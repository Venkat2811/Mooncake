# SharedQuotaManager Refactor Design

**Status**: Design Document
**Target**: Replace manual process-shared mutex with Flow-IPC session pattern
**Impact**: Eliminate ~50 lines of boilerplate, automatic dead-process cleanup

## Current Implementation Analysis

### Problems with Current Approach

**File**: `tent/src/transport/rdma/shared_quota.cpp`

#### 1. Manual Robust Mutex Handling (lines 104-150)

```cpp
Status SharedQuotaManager::initMutex(pthread_mutex_t* m) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

    #if defined(PTHREAD_MUTEX_ROBUST)
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);  // ← Complex
    #endif

    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return Status::OK();
}

int SharedQuotaManager::lock() {
    int rc = pthread_mutex_lock(&hdr_->global_mutex);
    if (rc == EOWNERDEAD) {
        #if defined(PTHREAD_MUTEX_ROBUST)
        pthread_mutex_consistent(&hdr_->global_mutex);  // ← Manual recovery
        #endif
        reclaimDeadPidsInternal();  // ← Manual cleanup
        return 0;
    }
    return rc;
}
```

**Issues**:
- ~30 lines of mutex boilerplate
- Platform-specific `#ifdef` checks
- Manual EOWNERDEAD handling
- Complex error recovery

#### 2. Manual Dead PID Detection (lines 152-177)

```cpp
void SharedQuotaManager::reclaimDeadPidsInternal() {
    for (int i = 0; i < hdr_->num_devices; ++i) {
        SharedDeviceEntry& dev = hdr_->devices[i];
        for (int s = 0; s < MAX_PID_SLOTS; ++s) {
            pid_t p = dev.pid_usages[s].pid;
            if (p == 0) continue;
            if (!isPidAlive(p)) {  // ← Manual probing
                dev.pid_usages[s].pid = 0;
                dev.pid_usages[s].used_bytes = 0;
            }
        }
    }
}

bool SharedQuotaManager::isPidAlive(pid_t pid) {
    if (pid <= 0) return false;
    int r = kill(pid, 0);  // ← Probe with signal
    if (r == 0) return true;
    if (errno == ESRCH) return false;
    return true;
}
```

**Issues**:
- Manual liveness checking
- Inefficient (scans all PIDs)
- No proactive notification of crashes

#### 3. Raw Struct in SHM (shared_quota.h:43-61)

```cpp
struct SharedHeader {
    uint64_t magic;
    int32_t version;
    int32_t num_devices;
    pthread_mutex_t global_mutex;  // ← Platform-specific
    SharedDeviceEntry devices[MAX_DEVICES];
};
```

**Issues**:
- No type safety
- Manual serialization/padding
- Version management is manual
- ABI fragility across platforms

## Flow-IPC Solution

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│ Current: Manual SHM + Robust Mutex                          │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Process A          SharedHeader (SHM)        Process B     │
│  ┌──────┐          ┌──────────────┐          ┌──────┐      │
│  │ lock()├─────────►│ pthread_mutex│◄─────────┤lock()│      │
│  │      │          │   (ROBUST)   │          │      │      │
│  │ A    │          │              │          │  B   │      │
│  │ dies │          │ EOWNERDEAD!  │          │      │      │
│  │  ☠   │          │              │          │checks│      │
│  │      │          │ kill(A, 0)?  │◄─────────┤alive │      │
│  └──────┘          └──────────────┘          └──────┘      │
│                                                              │
│  Manual cleanup, polling, complex error handling            │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ Flow-IPC: Session-Based IPC                                 │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Process A          Session Server           Process B      │
│  ┌──────┐          ┌──────────────┐          ┌──────┐      │
│  │Client│◄────────►│   Server     │◄────────►│Client│      │
│  │Session          │   Tracks     │          │Session      │
│  │      │  update  │   Sessions   │  query   │      │      │
│  │ A    ├─────────►│              │◄─────────┤  B   │      │
│  │ dies │          │              │          │      │      │
│  │  ☠   │          │ Session A    │          │      │      │
│  │      │          │ auto-removed │          │      │      │
│  └──────┘          └──────────────┘          └──────┘      │
│                                                              │
│  Automatic cleanup, event-driven, type-safe messages        │
└─────────────────────────────────────────────────────────────┘
```

### Key Improvements

| Aspect | Current | With Flow-IPC |
|--------|---------|---------------|
| **Process tracking** | Manual kill() probe | Session lifecycle |
| **Dead process cleanup** | Manual scan in EOWNERDEAD | Automatic on session end |
| **Synchronization** | Robust mutex + manual recovery | Message passing |
| **Data format** | Raw struct in SHM | Cap'n Proto schema |
| **Error handling** | Manual EOWNERDEAD, errno | Status codes, callbacks |
| **Code complexity** | ~150 LOC | ~50 LOC (estimate) |

## Proposed Implementation

### Phase 1: Define Cap'n Proto Schema

```capnp
# quota_messages.capnp

struct QuotaUpdate {
    deviceName @0 :Text;
    usedBytes @1 :UInt64;
    pid @2 :UInt32;
}

struct QuotaQuery {
    deviceName @0 :Text;
}

struct QuotaResponse {
    deviceName @0 :Text;
    activeBytes @1 :UInt64;
    numProcesses @2 :UInt32;
}
```

**Benefits**:
- Type-safe messages
- Automatic serialization
- Version evolution support
- Cross-platform ABI

### Phase 2: Session-Based Architecture

```cpp
// New file: shared_quota_v2.h

#include "tent/transport/shm/shm_arena.h"  // Reuse our arena!

class SharedQuotaServer {
public:
    Status start(const std::string& server_name);
    Status stop();

    // Called when client session ends (process dies)
    void onClientDisconnected(uint32_t client_id);

private:
    // Track quota per device
    struct DeviceState {
        std::string name;
        std::atomic<uint64_t> active_bytes{0};
        std::unordered_map<uint32_t, uint64_t> client_quotas;
    };

    std::unordered_map<std::string, DeviceState> devices_;
    std::mutex devices_mutex_;

    // Session management (Flow-IPC style)
    // In full implementation, would use actual Flow-IPC sessions
    std::unordered_map<uint32_t, std::string> client_sessions_;
};

class SharedQuotaClient {
public:
    Status connect(const std::string& server_name);
    Status updateQuota(const std::string& device, uint64_t used_bytes);
    Status queryQuota(const std::string& device, uint64_t& active_bytes);

private:
    uint32_t client_id_;
    // Connection to server
    // In full implementation, would use Flow-IPC client session
};
```

### Phase 3: Simplified Implementation

**Automatic cleanup on process death**:

```cpp
void SharedQuotaServer::onClientDisconnected(uint32_t client_id) {
    // Automatically called when client process dies
    std::lock_guard<std::mutex> lock(devices_mutex_);

    // Remove all quotas for this client
    for (auto& [device_name, device] : devices_) {
        auto it = device.client_quotas.find(client_id);
        if (it != device.client_quotas.end()) {
            uint64_t released = it->second;
            device.active_bytes.fetch_sub(released);
            device.client_quotas.erase(it);

            LOG(INFO) << "Auto-released " << released << " bytes from device "
                      << device_name << " (client " << client_id << " died)";
        }
    }

    client_sessions_.erase(client_id);
}
```

**No manual liveness checking needed!**

### Phase 4: Integration

```cpp
// Replace SharedQuotaManager with SharedQuotaClient

// Old code:
SharedQuotaManager quota_mgr(&local_quota);
quota_mgr.attach("/mooncake_quota");
quota_mgr.diffusion();

// New code:
SharedQuotaClient quota_client;
quota_client.connect("mooncake_quota_server");
quota_client.updateQuota("mlx5_0", used_bytes);
```

## Code Comparison

### Current: ~150 lines of complex code

```cpp
// Initialize mutex
Status initMutex(pthread_mutex_t* m) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    #if defined(PTHREAD_MUTEX_ROBUST)
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    #endif
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return Status::OK();
}

// Lock with recovery
int lock() {
    int rc = pthread_mutex_lock(&hdr_->global_mutex);
    if (rc == EOWNERDEAD) {
        #if defined(PTHREAD_MUTEX_ROBUST)
        pthread_mutex_consistent(&hdr_->global_mutex);
        #endif
        reclaimDeadPidsInternal();
        return 0;
    }
    return rc;
}

// Manual dead PID cleanup
void reclaimDeadPidsInternal() {
    for (int i = 0; i < hdr_->num_devices; ++i) {
        SharedDeviceEntry& dev = hdr_->devices[i];
        for (int s = 0; s < MAX_PID_SLOTS; ++s) {
            pid_t p = dev.pid_usages[s].pid;
            if (p == 0) continue;
            if (!isPidAlive(p)) {
                dev.pid_usages[s].pid = 0;
                dev.pid_usages[s].used_bytes = 0;
            }
        }
    }
}

// Probe each PID
bool isPidAlive(pid_t pid) {
    int r = kill(pid, 0);
    if (r == 0) return true;
    if (errno == ESRCH) return false;
    return true;
}
```

### Proposed: ~50 lines of clean code

```cpp
// Update quota (simple!)
Status updateQuota(const std::string& device, uint64_t used_bytes) {
    QuotaUpdate update;
    update.deviceName = device;
    update.usedBytes = used_bytes;
    update.pid = getpid();

    // Send via session channel (automatic serialization)
    return sendMessage(update);
}

// Automatic cleanup on disconnect (event-driven!)
void onClientDisconnected(uint32_t client_id) {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    for (auto& [device_name, device] : devices_) {
        auto it = device.client_quotas.find(client_id);
        if (it != device.client_quotas.end()) {
            device.active_bytes.fetch_sub(it->second);
            device.client_quotas.erase(it);
        }
    }
}
```

**Eliminated**:
- ✅ Manual robust mutex initialization
- ✅ EOWNERDEAD handling
- ✅ Manual dead PID scanning
- ✅ kill() probing
- ✅ Platform-specific #ifdefs
- ✅ Raw struct serialization

**Result**: **~100 lines eliminated**, cleaner code, automatic cleanup

## Migration Strategy

### Step 1: Prototype (Low Risk)

Create `shared_quota_v2.{h,cpp}` alongside existing implementation:

```bash
tent/include/tent/transport/rdma/
├── shared_quota.h          # Keep existing
└── shared_quota_v2.h       # New implementation

tent/src/transport/rdma/
├── shared_quota.cpp        # Keep existing
└── shared_quota_v2.cpp     # New implementation
```

### Step 2: A/B Testing

```cpp
#ifdef USE_QUOTA_V2
    SharedQuotaClient quota(&local_quota);
    quota.connect("mooncake_quota");
#else
    SharedQuotaManager quota(&local_quota);
    quota.attach("/mooncake_quota");
#endif
```

### Step 3: Gradual Rollout

1. Deploy v2 to 10% of machines
2. Monitor for issues
3. Expand to 50%
4. Full rollout
5. Remove v1 after validation period

## Benefits Summary

### Code Quality
- **-100 LOC**: Eliminate boilerplate
- **+Type Safety**: Cap'n Proto schemas
- **+Testability**: Mock sessions easily
- **+Maintainability**: No platform-specific code

### Reliability
- **Auto Cleanup**: Dead processes handled automatically
- **No Polling**: Event-driven instead of periodic scans
- **Proper Errors**: Structured status codes vs errno

### Performance
- **Lower Latency**: No kill() probes (can be slow)
- **Better Scalability**: No O(n) PID scans
- **Lock-Free Paths**: Message passing vs mutex contention

## Implementation Estimate

**Effort**: ~1-2 days
**Risk**: Low (v2 alongside v1)
**Impact**: High (cleaner, more reliable)

### Task Breakdown

1. **Define schema** (2 hours)
   - Write Cap'n Proto definitions
   - Generate C++ bindings

2. **Implement server** (4 hours)
   - Session lifecycle management
   - Quota tracking per device
   - Cleanup on disconnect

3. **Implement client** (2 hours)
   - Connection management
   - Update/query operations

4. **Testing** (4 hours)
   - Unit tests
   - Multi-process tests
   - Crash recovery tests

5. **Integration** (4 hours)
   - Wire into existing RDMA code
   - A/B testing setup

## Conclusion

The SharedQuotaManager refactor demonstrates Flow-IPC's key value proposition:

**Replace manual IPC plumbing with high-level abstractions**

- Manual → Automatic (cleanup, serialization)
- Complex → Simple (~150 LOC → ~50 LOC)
- Error-Prone → Robust (no manual recovery)

This same pattern can be applied to other IPC-heavy components in Mooncake.

---

**Next Steps**:
1. Review this design with team
2. Prototype SharedQuotaV2 (1-2 days)
3. A/B test in dev environment
4. Gradual production rollout

**Status**: Ready for implementation
