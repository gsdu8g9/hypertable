/**
 * Copyright (C) 2007 Doug Judd (Zvents, Inc.)
 * 
 * This file is part of Hypertable.
 * 
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 * 
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <algorithm>
#include <cstring>

extern "C" {
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
}

#include "Common/Error.h"
#include "Common/FileUtils.h"
#include "Common/System.h"

#include "DfsBroker/Lib/Client.h"
#include "Hypertable/Lib/Schema.h"

#include "Event.h"
#include "Notification.h"
#include "Master.h"
#include "Session.h"
#include "SessionData.h"

using namespace hypertable;
using namespace Hyperspace;
using namespace std;

const uint32_t Master::DEFAULT_MASTER_PORT;
const uint32_t Master::DEFAULT_LEASE_INTERVAL;
const uint32_t Master::DEFAULT_KEEPALIVE_INTERVAL;

/**
 *
 */
Master::Master(ConnectionManager *connManager, PropertiesPtr &propsPtr, ServerKeepaliveHandlerPtr &keepaliveHandlerPtr) : mVerbose(false), mNextHandleNumber(1), mNextSessionId(1), mNextEventId(1) {
  const char *dirname;
  std::string str;
  ssize_t xattrLen;
  uint16_t port;

  mVerbose = propsPtr->getPropertyBool("verbose", false);

  mLeaseInterval = (uint32_t)propsPtr->getPropertyInt("Hyperspace.Lease.Interval", DEFAULT_LEASE_INTERVAL);

  mKeepAliveInterval = (uint32_t)propsPtr->getPropertyInt("Hyperspace.KeepAlive.Interval", DEFAULT_KEEPALIVE_INTERVAL);

  if ((dirname = propsPtr->getProperty("Hyperspace.Master.dir", 0)) == 0) {
    LOG_ERROR("Property 'Hyperspace.Master.dir' not found.");
    exit(1);
  }

  if (dirname[strlen(dirname)-1] == '/')
    str = std::string(dirname, strlen(dirname)-1);
  else
    str = dirname;

  if (dirname[0] == '/')
    mBaseDir = str;
  else
    mBaseDir = System::installDir + "/" + str;

  if ((mBaseFd = open(mBaseDir.c_str(), O_RDONLY)) < 0) {
    LOG_VA_ERROR("Unable to open base directory %s - %s", mBaseDir.c_str(), strerror(errno));
    exit(1);
  }

  /**
   * Lock the base directory to prevent concurrent masters
   */
  if (flock(mBaseFd, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      LOG_VA_ERROR("Base directory '%s' is locked by another process.", mBaseDir.c_str());
    }
    else {
      LOG_VA_ERROR("Unable to lock base directory '%s' - %s", mBaseDir.c_str(), strerror(errno));
    }
    exit(1);
  }

  /**
   * Increment generation number and save
   */
  if ((xattrLen = FileUtils::Getxattr(mBaseDir.c_str(), "generation", &mGeneration, sizeof(uint32_t))) < 0) {
    if (errno == ENOATTR) {
      cerr << "'generation' attribute not found on base dir, creating ..." << endl;
      mGeneration = 1;
      if (FileUtils::Setxattr(mBaseDir.c_str(), "generation", &mGeneration, sizeof(uint32_t), XATTR_CREATE) == -1) {
	LOG_VA_ERROR("Problem creating extended attribute 'generation' on base dir '%s' - %s", mBaseDir.c_str(), strerror(errno));
	exit(1);
      }
    }
    else {
      LOG_VA_ERROR("Unable to read extended attribute 'generation' on base dir '%s' - %s", mBaseDir.c_str(), strerror(errno));
      exit(1);
    }
  }
  else {
    mGeneration++;
    if (FileUtils::Setxattr(mBaseDir.c_str(), "generation", &mGeneration, sizeof(uint32_t), XATTR_REPLACE) == -1) {
      LOG_VA_ERROR("Problem creating extended attribute 'generation' on base dir '%s' - %s", mBaseDir.c_str(), strerror(errno));
      exit(1);
    }
  }

  port = propsPtr->getPropertyInt("Hyperspace.Master.port", DEFAULT_MASTER_PORT);
  InetAddr::Initialize(&mLocalAddr, INADDR_ANY, port);

  mKeepaliveHandlerPtr.reset( new ServerKeepaliveHandler(connManager->GetComm(), this) );
  keepaliveHandlerPtr = mKeepaliveHandlerPtr;

  if (mVerbose) {
    cout << "Hyperspace.Lease.Interval=" << mLeaseInterval << endl;
    cout << "Hyperspace.KeepAlive.Interval=" << mKeepAliveInterval << endl;
    cout << "Hyperspace.Master.dir=" << mBaseDir << endl;
    cout << "Generation=" << mGeneration << endl;
  }

}


Master::~Master() {
  close(mBaseFd);
  return;
}


uint64_t Master::CreateSession(struct sockaddr_in &addr) {
  boost::mutex::scoped_lock lock(mSessionMapMutex);
  SessionDataPtr sessionPtr;
  uint64_t sessionId = mNextSessionId++;
  sessionPtr = new SessionData(addr, mLeaseInterval, sessionId);
  mSessionMap[sessionId] = sessionPtr;
  mSessionHeap.push_back(sessionPtr);
  return sessionId;
}


bool Master::GetSessionData(uint64_t sessionId, SessionDataPtr &sessionPtr) {
  boost::mutex::scoped_lock lock(mSessionMapMutex);
  SessionMapT::iterator iter = mSessionMap.find(sessionId);
  if (iter == mSessionMap.end())
    return false;
  sessionPtr = (*iter).second;
  return true;
}


int Master::RenewSessionLease(uint64_t sessionId) {
  boost::mutex::scoped_lock lock(mSessionMapMutex);  
  boost::xtime now;

  SessionMapT::iterator iter = mSessionMap.find(sessionId);
  if (iter == mSessionMap.end())
    return Error::HYPERSPACE_EXPIRED_SESSION;

  if (!(*iter).second->RenewLease())
    return Error::HYPERSPACE_EXPIRED_SESSION;

  return Error::OK;
}

bool Master::NextExpiredSession(SessionDataPtr &sessionPtr) {
  boost::mutex::scoped_lock lock(mSessionMapMutex);
  struct ltSessionData compFunc;
  boost::xtime now;

  boost::xtime_get(&now, boost::TIME_UTC);

  if (mSessionHeap.size() > 0) {
    std::make_heap(mSessionHeap.begin(), mSessionHeap.end(), compFunc);
    if (mSessionHeap[0]->IsExpired(now)) {
      sessionPtr = mSessionHeap[0];
      std::pop_heap(mSessionHeap.begin(), mSessionHeap.end(), compFunc);
      mSessionHeap.resize(mSessionHeap.size()-1);
      return true;
    }    
  }
  return false;
}

void Master::RemoveExpiredSessions() {
  SessionDataPtr sessionPtr;
  
  while (NextExpiredSession(sessionPtr)) {
    if (mVerbose) {
      LOG_VA_INFO("Expiring session %lld", sessionPtr->id);
    }
    sessionPtr->Expire();

    {
      boost::mutex::scoped_lock slock(sessionPtr->mutex);
      HandleDataPtr handlePtr;
      for (set<uint64_t>::iterator iter = sessionPtr->handles.begin(); iter != sessionPtr->handles.end(); iter++) {
	if (mVerbose) {
	  LOG_VA_INFO("Destroying handle %lld", *iter);
	}
	if (!RemoveHandleData(*iter, handlePtr)) {
	  LOG_VA_ERROR("Expired session handle %lld invalid", *iter);
	}
	else if (!DestroyHandle(handlePtr, false)) {
	  LOG_VA_ERROR("Problem destroying handle %lld of expired session - %s", *iter, strerror(errno));
	}
      }
    }
  }

}


/**
 *
 */
void Master::CreateHandle(uint64_t *handlep, HandleDataPtr &handlePtr) {
  boost::mutex::scoped_lock lock(mHandleMapMutex);
  *handlep = ++mNextHandleNumber;
  handlePtr = new HandleData();
  handlePtr->id = *handlep;
  handlePtr->locked = false;
  mHandleMap[*handlep] = handlePtr;
}

/**
 *
 */
bool Master::GetHandleData(uint64_t handle, HandleDataPtr &handlePtr) {
  boost::mutex::scoped_lock lock(mHandleMapMutex);
  HandleMapT::iterator iter = mHandleMap.find(handle);
  if (iter == mHandleMap.end())
    return false;
  handlePtr = (*iter).second;
  return true;
}

/**
 *
 */
bool Master::RemoveHandleData(uint64_t handle, HandleDataPtr &handlePtr) {
  boost::mutex::scoped_lock lock(mHandleMapMutex);
  HandleMapT::iterator iter = mHandleMap.find(handle);
  if (iter == mHandleMap.end())
    return false;
  handlePtr = (*iter).second;
  mHandleMap.erase(iter);
  return true;
}



/**
 * Mkdir
 */
void Master::Mkdir(ResponseCallback *cb, uint64_t sessionId, const char *name) {
  std::string absName;

  if (mVerbose) {
    LOG_VA_INFO("mkdir(sessionId=%lld, name=%s)", sessionId, name);
  }

  assert(name[0] == '/' && name[strlen(name)-1] != '/');

  absName = mBaseDir + name;

  if (mkdir(absName.c_str(), 0755) < 0)
    ReportError(cb);
  else {
    NodeDataPtr parentNodePtr;
    std::string childName;

    if (FindParentNode(name, parentNodePtr, childName)) {
      HyperspaceEventPtr eventPtr( new EventNamed(mNextEventId++, EVENT_MASK_CHILD_NODE_ADDED, childName) );
      DeliverEventNotifications(parentNodePtr.get(), eventPtr);
    }
    cb->response_ok();
  }

  return;
}


/**
 * Delete
 */
void Master::Delete(ResponseCallback *cb, uint64_t sessionId, const char *name) {
  std::string absName;
  struct stat statbuf;

  if (mVerbose) {
    LOG_VA_INFO("delete(sessionId=%lld, name=%s)", sessionId, name);
  }

  assert(name[0] == '/' && name[strlen(name)-1] != '/');

  absName = mBaseDir + name;

  if (stat(absName.c_str(), &statbuf) < 0) {
    ReportError(cb);
    return;
  }
  else {
    if (statbuf.st_mode & S_IFDIR) {
      if (rmdir(absName.c_str()) < 0) {
	ReportError(cb);
	return;
      }
    }
    else {
      if (unlink(absName.c_str()) < 0) {
	ReportError(cb);
	return;
      }
    }
  }

  // deliver notifications
  {
    std::string childName;
    NodeDataPtr parentNodePtr;

    if (FindParentNode(name, parentNodePtr, childName)) {
      HyperspaceEventPtr eventPtr( new EventNamed(mNextEventId++, EVENT_MASK_CHILD_NODE_REMOVED, childName) );
      DeliverEventNotifications(parentNodePtr.get(), eventPtr);
    }
  }

  cb->response_ok();
}



/**
 * Open
 */
void Master::Open(ResponseCallbackOpen *cb, uint64_t sessionId, const char *name, uint32_t flags, uint32_t eventMask) {
  std::string absName;
  SessionDataPtr sessionPtr;
  NodeDataPtr nodePtr;
  HandleDataPtr handlePtr;
  int error;
  bool created = false;
  bool existed;
  uint64_t handle;
  struct stat statbuf;
  int oflags = 0;
  NodeMapT::iterator nodeIter;

  if (mVerbose) {
    LOG_VA_INFO("open(sessionId=%lld, fname=%s, flags=0x%x, eventMask=0x%x)", sessionId, name, flags, eventMask);
  }

  assert(name[0] == '/' && name[strlen(name)-1] != '/');

  absName = mBaseDir + name;

  if (!GetSessionData(sessionId, sessionPtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  {
    boost::mutex::scoped_lock lock(mNodeMapMutex);
    int fd;

    nodeIter = mNodeMap.find(name);
    if (nodeIter != mNodeMap.end()) {
      if ((flags & OPEN_FLAG_CREATE) && (flags & OPEN_FLAG_EXCL)) {
	cb->error(Error::HYPERSPACE_FILE_EXISTS, "mode=CREATE|EXCL");
	return;
      }
      nodePtr = (*nodeIter).second;
    }

    if (stat(absName.c_str(), &statbuf) != 0) {
      if (errno == ENOENT)
	existed = false;
      else {
	ReportError(cb);
	return;
      }
    }
    else {
      existed = true;
      if (statbuf.st_mode & S_IFDIR) {
	/** if (flags & OPEN_FLAG_WRITE) {
	    cb->error(Error::HYPERSPACE_IS_DIRECTORY, "");
	    return;
	    } **/
	oflags = O_RDONLY;
      }
      else
	oflags = O_RDWR;
    }

    if (!nodePtr || nodePtr->fd < 0) {
      if (nodePtr && (flags & OPEN_FLAG_TEMP) && existed && !nodePtr->ephemeral) {
	cb->error(Error::HYPERSPACE_FILE_EXISTS, "Unable to open TEMP file because it exists and is permanent");
	return;
      }
      if (flags & OPEN_FLAG_CREATE)
	oflags |= O_CREAT;
      if (flags & OPEN_FLAG_EXCL)
	oflags |= O_EXCL;
      fd = open(absName.c_str(), oflags, 0644);
      if (fd < 0) {
	ReportError(cb);
	return;
      }
      if (!nodePtr) {
	ssize_t len;
	nodePtr = new NodeData();
	nodePtr->name = name;
	len = FileUtils::Fgetxattr(fd, "lock.generation", &nodePtr->lockGeneration, sizeof(uint64_t));
	if (len < 0) {
	  if (errno == ENOATTR) {
	    nodePtr->lockGeneration = 1;
	    if (FileUtils::Fsetxattr(fd, "lock.generation", &nodePtr->lockGeneration, sizeof(uint64_t), 0) == -1) {
	      ReportError(cb);
	      LOG_VA_ERROR("Problem creating extended attribute 'lock.generation' on file '%s' - %s",
			   name, strerror(errno));
	      return;
	    }
	  }
	  else {
	    ReportError(cb);
	    LOG_VA_ERROR("Problem reading extended attribute 'lock.generation' on file '%s' - %s",
			 name, strerror(errno));
	    return;
	  }
	  len = sizeof(int64_t);
	}
	assert(len == sizeof(int64_t));

	if (flags & OPEN_FLAG_TEMP) {
	  nodePtr->ephemeral = true;
	  unlink(absName.c_str());
	}
	mNodeMap[name] = nodePtr;
      }
      nodePtr->fd = fd;
      if (!existed)
	created = true;
    }

    CreateHandle(&handle, handlePtr);

    handlePtr->node = nodePtr.get();
    handlePtr->openFlags = flags;
    handlePtr->eventMask = eventMask;
    handlePtr->sessionPtr = sessionPtr;

    sessionPtr->AddHandle(handle);

    {
      std::string childName;

      if (created && FindParentNode(name, nodePtr, childName)) {
	HyperspaceEventPtr eventPtr( new EventNamed(mNextEventId++, EVENT_MASK_CHILD_NODE_ADDED, childName) );
	DeliverEventNotifications(nodePtr.get(), eventPtr);
      }
    }

    handlePtr->node->AddHandle(handle, handlePtr);
  }

  cb->response(handle, created);
  
}


void Master::Close(ResponseCallback *cb, uint64_t sessionId, uint64_t handle) {
  SessionDataPtr sessionPtr;
  HandleDataPtr handlePtr;
  int error;

  if (mVerbose) {
    LOG_VA_INFO("close(session=%lld, handle=%lld)", sessionId, handle);
  }

  if (!GetSessionData(sessionId, sessionPtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (!RemoveHandleData(handle, handlePtr)) {
    cb->error(Error::HYPERSPACE_INVALID_HANDLE, "");
    return;
  }

  if (!DestroyHandle(handlePtr)) {
    ReportError(cb);
    return;
  }

  if ((error = cb->response_ok()) != Error::OK) {
    LOG_VA_ERROR("Problem sending back response - %s", Error::GetText(error));
  }
}



/**
 *
 */
void Master::AttrSet(ResponseCallback *cb, uint64_t sessionId, uint64_t handle, const char *name, const void *value, size_t valueLen) {
  SessionDataPtr sessionPtr;
  HandleDataPtr handlePtr;
  int error;

  if (mVerbose) {
    LOG_VA_INFO("attrset(session=%lld, handle=%lld, name=%s, valueLen=%d)", sessionId, handle, name, valueLen);
  }

  if (!GetSessionData(sessionId, sessionPtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (!GetHandleData(handle, handlePtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (FileUtils::Fsetxattr(handlePtr->node->fd, name, value, valueLen, 0) == -1) {
    LOG_VA_ERROR("Problem creating extended attribute '%s' on file '%s' - %s",
		 name, handlePtr->node->name.c_str(), strerror(errno));
    exit(1);
  }

  {
    HyperspaceEventPtr eventPtr( new EventNamed(mNextEventId++, EVENT_MASK_ATTR_SET, name) );
    DeliverEventNotifications(handlePtr->node, eventPtr);
  }

  if ((error = cb->response_ok()) != Error::OK) {
    LOG_VA_ERROR("Problem sending back response - %s", Error::GetText(error));
  }

}


/**
 * 
 */
void Master::AttrGet(ResponseCallbackAttrGet *cb, uint64_t sessionId, uint64_t handle, const char *name) {
  SessionDataPtr sessionPtr;
  HandleDataPtr handlePtr;
  int error;
  int alen;
  uint8_t *buf;

  if (mVerbose) {
    LOG_VA_INFO("attrget(session=%lld, handle=%lld, name=%s)", sessionId, handle, name);
  }

  if (!GetSessionData(sessionId, sessionPtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (!GetHandleData(handle, handlePtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if ((alen = FileUtils::Fgetxattr(handlePtr->node->fd, name, 0, 0)) < 0) {
    ReportError(cb);
    LOG_VA_ERROR("Problem determining size of extended attribute '%s' on file '%s' - %s",
		 name, handlePtr->node->name.c_str(), strerror(errno));
    return;
  }

  buf = new uint8_t [ alen + 8 ];

  if ((alen = FileUtils::Fgetxattr(handlePtr->node->fd, name, buf, alen)) < 0) {
    ReportError(cb);
    LOG_VA_ERROR("Problem determining size of extended attribute '%s' on file '%s' - %s",
		 name, handlePtr->node->name.c_str(), strerror(errno));
    return;
  }

  if ((error = cb->response(buf, (uint32_t)alen)) != Error::OK) {
    LOG_VA_ERROR("Problem sending back response - %s", Error::GetText(error));
  }
  
}



void Master::AttrDel(ResponseCallback *cb, uint64_t sessionId, uint64_t handle, const char *name) {
  SessionDataPtr sessionPtr;
  HandleDataPtr handlePtr;
  int error;

  if (mVerbose) {
    LOG_VA_INFO("attrdel(session=%lld, handle=%lld, name=%s)", sessionId, handle, name);
  }

  if (!GetSessionData(sessionId, sessionPtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (!GetHandleData(handle, handlePtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (FileUtils::Fremovexattr(handlePtr->node->fd, name) == -1) {
    ReportError(cb);
    LOG_VA_ERROR("Problem removing extended attribute '%s' on file '%s' - %s",
		 name, handlePtr->node->name.c_str(), strerror(errno));
    return;
  }

  {
    HyperspaceEventPtr eventPtr( new EventNamed(mNextEventId++, EVENT_MASK_ATTR_DEL, name) );
    DeliverEventNotifications(handlePtr->node, eventPtr);
  }

  if ((error = cb->response_ok()) != Error::OK) {
    LOG_VA_ERROR("Problem sending back response - %s", Error::GetText(error));
  }

}


void Master::Exists(ResponseCallbackExists *cb, uint64_t sessionId, const char *name) {
  std::string absName;
  int error;

  if (mVerbose) {
    LOG_VA_INFO("exists(sessionId=%lld, name=%s)", sessionId, name);
  }

  assert(name[0] == '/' && name[strlen(name)-1] != '/');

  absName = mBaseDir + name;

  if ((error = cb->response( FileUtils::Exists(absName.c_str()) )) != Error::OK) {
    LOG_VA_ERROR("Problem sending back response - %s", Error::GetText(error));
  }
}


void Master::Lock(ResponseCallbackLock *cb, uint64_t sessionId, uint64_t handle, uint32_t mode, bool tryAcquire) {
  SessionDataPtr sessionPtr;
  HandleDataPtr handlePtr;
  int error;
  bool notify = true;

  if (mVerbose) {
    LOG_VA_INFO("lock(session=%lld, handle=%lld, mode=0x%x, tryAcquire=%d)", sessionId, handle, mode, tryAcquire);
  }

  if (!GetSessionData(sessionId, sessionPtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (!GetHandleData(handle, handlePtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (!(handlePtr->openFlags & OPEN_FLAG_LOCK)) {
    cb->error(Error::HYPERSPACE_MODE_RESTRICTION, "handle not open for locking");
    return;
  }

  if (!(handlePtr->openFlags & OPEN_FLAG_WRITE)) {
    cb->error(Error::HYPERSPACE_MODE_RESTRICTION, "handle not open for writing");
    return;
  }

  {
    boost::mutex::scoped_lock lock(handlePtr->node->mutex);

    if (handlePtr->node->currentLockMode == LOCK_MODE_EXCLUSIVE) {
      if (tryAcquire)
	cb->response(LOCK_STATUS_BUSY);
      else {
	handlePtr->node->pendingLockRequests.push_back( LockRequest(handle, mode) );
	cb->response(LOCK_STATUS_PENDING);
      }
      return;
    }
    else if (handlePtr->node->currentLockMode == LOCK_MODE_SHARED) {
      if (mode == LOCK_MODE_EXCLUSIVE) {
	if (tryAcquire)
	  cb->response(LOCK_STATUS_BUSY);
	else {
	  handlePtr->node->pendingLockRequests.push_back( LockRequest(handle, mode) );
	  cb->response(LOCK_STATUS_PENDING);
	}
	return;
      }

      assert(mode == LOCK_MODE_SHARED);

      if (!handlePtr->node->pendingLockRequests.empty()) {
	handlePtr->node->pendingLockRequests.push_back( LockRequest(handle, mode) );
	cb->response(LOCK_STATUS_PENDING);
	return;
      }
    }

    // at this point we're OK to acquire the lock

    if (mode == LOCK_MODE_SHARED && !handlePtr->node->sharedLockHandles.empty())
      notify = false;

    handlePtr->node->lockGeneration++;
    if (FileUtils::Fsetxattr(handlePtr->node->fd, "lock.generation", &handlePtr->node->lockGeneration, sizeof(uint64_t), 0) == -1) {
      LOG_VA_ERROR("Problem creating extended attribute 'lock.generation' on file '%s' - %s",
		   handlePtr->node->name.c_str(), strerror(errno));
      exit(1);
    }
    handlePtr->node->currentLockMode = mode;

    LockHandle(handlePtr, mode);

    // deliver notification to handles to this same node
    if (notify) {
      HyperspaceEventPtr eventPtr( new EventLockAcquired(mNextEventId++, mode) );
      DeliverEventNotifications(handlePtr->node, eventPtr);
    }

    cb->response(LOCK_STATUS_GRANTED, handlePtr->node->lockGeneration);
  }
}

/**
 * Assumes node is locked.
 */
void Master::LockHandle(HandleDataPtr &handlePtr, uint32_t mode, bool waitForNotify) {
  if (mode == LOCK_MODE_SHARED)
    handlePtr->node->sharedLockHandles.insert(handlePtr->id);
  else {
    assert(mode == LOCK_MODE_EXCLUSIVE);
    handlePtr->node->exclusiveLockHandle = handlePtr->id;
  }
  handlePtr->locked = true;
}

/**
 * Assumes node is locked.
 */
void Master::LockHandleWithNotification(HandleDataPtr &handlePtr, uint32_t mode, bool waitForNotify) {

  LockHandle(handlePtr, mode, waitForNotify);

  // deliver notification to handles to this same node
  {
    HyperspaceEventPtr eventPtr( new EventLockGranted(mNextEventId++, mode, handlePtr->node->lockGeneration) );
    DeliverEventNotification(handlePtr, eventPtr, waitForNotify);
  }

}



/**
 * 
 */
void Master::Release(ResponseCallback *cb, uint64_t sessionId, uint64_t handle) {
  SessionDataPtr sessionPtr;
  HandleDataPtr handlePtr;

  if (mVerbose) {
    LOG_VA_INFO("release(session=%lld, handle=%lld)", sessionId, handle);
  }

  if (!GetSessionData(sessionId, sessionPtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  if (!GetHandleData(handle, handlePtr)) {
    cb->error(Error::HYPERSPACE_EXPIRED_SESSION, "");
    return;
  }

  ReleaseLock(handlePtr);

  cb->response_ok();

}


void Master::ReleaseLock(HandleDataPtr &handlePtr, bool waitForNotify) {
  boost::mutex::scoped_lock lock(handlePtr->node->mutex);
  vector<HandleDataPtr> nextLockHandles;
  int nextMode = 0;

  if (handlePtr->locked) {
    if (handlePtr->node->exclusiveLockHandle != 0) {
      assert(handlePtr->id == handlePtr->node->exclusiveLockHandle);
      handlePtr->node->exclusiveLockHandle = 0;
    }
    else {
      unsigned int count = handlePtr->node->sharedLockHandles.erase(handlePtr->id);
      assert(count);
    }
    handlePtr->locked = false;
  }
  else
    return;

  // deliver LOCK_RELEASED notifications if no more locks held on node
  if (handlePtr->node->sharedLockHandles.empty()) {
    HyperspaceEventPtr eventPtr( new EventLockReleased(mNextEventId++) );
    DeliverEventNotifications(handlePtr->node, eventPtr, waitForNotify);
  }

  handlePtr->node->currentLockMode = 0;

  if (!handlePtr->node->pendingLockRequests.empty()) {
    HandleDataPtr nextHandlePtr;
    const LockRequest &frontLockReq = handlePtr->node->pendingLockRequests.front();
    if (frontLockReq.mode == LOCK_MODE_EXCLUSIVE) {
      nextMode = LOCK_MODE_EXCLUSIVE;
      if (GetHandleData(frontLockReq.handle, nextHandlePtr))
	nextLockHandles.push_back(nextHandlePtr);
      handlePtr->node->pendingLockRequests.pop_front();
    }
    else {
      nextMode = LOCK_MODE_SHARED;
      do {
	const LockRequest &lockreq = handlePtr->node->pendingLockRequests.front();
	if (lockreq.mode != LOCK_MODE_SHARED)
	  break;
	if (GetHandleData(lockreq.handle, nextHandlePtr))
	  nextLockHandles.push_back(nextHandlePtr);
	handlePtr->node->pendingLockRequests.pop_front();
      } while (!handlePtr->node->pendingLockRequests.empty());
    }

    if (!nextLockHandles.empty()) {

      handlePtr->node->lockGeneration++;
      if (FileUtils::Fsetxattr(handlePtr->node->fd, "lock.generation", &handlePtr->node->lockGeneration, sizeof(uint64_t), 0) == -1) {
	LOG_VA_ERROR("Problem creating extended attribute 'lock.generation' on file '%s' - %s",
		     handlePtr->node->name.c_str(), strerror(errno));
	exit(1);
      }

      handlePtr->node->currentLockMode = nextMode;

      for (size_t i=0; i<nextLockHandles.size(); i++) {
	assert(handlePtr->id != nextLockHandles[i]->id);
	LockHandleWithNotification(nextLockHandles[i], nextMode, waitForNotify);
      }

      // deliver notification to handles to this same node
      {
	HyperspaceEventPtr eventPtr( new EventLockAcquired(mNextEventId++, nextMode) );
	DeliverEventNotifications(handlePtr->node, eventPtr, waitForNotify);
      }

    }

  }
  
}


/**
 * ReportError
 */
void Master::ReportError(ResponseCallback *cb) {
  char errbuf[128];
  errbuf[0] = 0;
  strerror_r(errno, errbuf, 128);
  if (errno == ENOTDIR || errno == ENAMETOOLONG || errno == ENOENT)
    cb->error(Error::HYPERSPACE_BAD_PATHNAME, errbuf);
  else if (errno == EACCES || errno == EPERM)
    cb->error(Error::HYPERSPACE_PERMISSION_DENIED, errbuf);
  else if (errno == EEXIST)
    cb->error(Error::HYPERSPACE_FILE_EXISTS, errbuf);
  else if (errno == ENOATTR)
    cb->error(Error::HYPERSPACE_ATTR_NOT_FOUND, errbuf);
  else
    cb->error(Error::HYPERSPACE_IO_ERROR, errbuf);
}



/**
 *
 */
void Master::NormalizeName(std::string name, std::string &normal) {
  normal = "";
  if (name[0] != '/')
    normal += "/";

  if (name.find('/', name.length()-1) == string::npos)
    normal += name;
  else
    normal += name.substr(0, name.length()-1);
}


/**
 * Assumes node is locked.
 */
void Master::DeliverEventNotifications(NodeData *node, HyperspaceEventPtr &eventPtr, bool waitForNotify) {
  int notifications = 0;

  // log event

  for (HandleMapT::iterator iter = node->handleMap.begin(); iter != node->handleMap.end(); iter++) {
    //LOG_VA_INFO("Delivering notification (%d == %d)", (*iter).second->eventMask, eventPtr->GetMask());
    if ((*iter).second->eventMask & eventPtr->GetMask()) {
      eventPtr->IncrementNotificationCount();
      (*iter).second->sessionPtr->AddNotification( new Notification((*iter).first, eventPtr) );
      mKeepaliveHandlerPtr->DeliverEventNotifications((*iter).second->sessionPtr->id);
      notifications++;
    }
  }

  if (waitForNotify && notifications)
    eventPtr->WaitForNotifications();
}


/**
 * Assumes node is locked.
 */
void Master::DeliverEventNotification(HandleDataPtr &handlePtr, HyperspaceEventPtr &eventPtr, bool waitForNotify) {
  int notifications = 0;

  // log event

  handlePtr->sessionPtr->AddNotification( new Notification(handlePtr->id, eventPtr) );
  mKeepaliveHandlerPtr->DeliverEventNotifications(handlePtr->sessionPtr->id);

  if (waitForNotify)
    eventPtr->WaitForNotifications();
}




/**
 *
 */
bool Master::FindParentNode(std::string normalName, NodeDataPtr &parentNodePtr, std::string &childName) {
  NodeMapT::iterator nodeIter;
  unsigned int lastSlash = normalName.rfind("/", normalName.length());
  std::string parentName;

  if (lastSlash > 0) {
    parentName = normalName.substr(0, lastSlash);
    childName = normalName.substr(lastSlash+1);
    nodeIter = mNodeMap.find(parentName);
    if (nodeIter != mNodeMap.end()) {
      parentNodePtr = (*nodeIter).second;
      return true;
    }
  }

  return false;
}


/**
 * TODO: Destory locks
 */
bool Master::DestroyHandle(HandleDataPtr &handlePtr, bool waitForNotify) {

  handlePtr->node->RemoveHandle(handlePtr->id);

  {
    boost::mutex::scoped_lock lock(mNodeMapMutex);

    if (handlePtr->node->ReferenceCount() == 0) {

      if (handlePtr->node->Close() != 0)
	return false;

      if (handlePtr->node->ephemeral) {
	std::string childName;
	NodeDataPtr parentNodePtr;

	if (FindParentNode(handlePtr->node->name, parentNodePtr, childName)) {
	  HyperspaceEventPtr eventPtr( new EventNamed(mNextEventId++, EVENT_MASK_CHILD_NODE_REMOVED, childName) );
	  DeliverEventNotifications(parentNodePtr.get(), eventPtr, waitForNotify);
	}

	// remove node
	NodeMapT::iterator nodeIter = mNodeMap.find(handlePtr->node->name);
	if (nodeIter != mNodeMap.end())
	  mNodeMap.erase(nodeIter);
      }
    }
  }

  return true;
}