/**
 * This file is part of the CernVM File System.
 *
 * This module provides three data structures to save "active inodes".
 * These are inodes with a reference counter > 0 in the VFS layer, which
 * can be asked for even if the caches are drained.  Such inodes must be
 * kept in buffers during a catalog reload and a reload of the fuse module,
 * since in these cases the inode generation changes and all current inodes
 * become invalid.
 *
 * The glue buffer saves inodes of "lookup chains" that happen to be cut
 * by a reload (i.e. stat() calls from user land perspective).
 * The cwd buffer saves all current directories of processes on the cvmfs
 * mount point.
 * The active inode buffer saves inodes from directories that are held open
 * (opendir) or that contain currently open files.
 *
 * These objects have to survive reloading of the library, so no virtual
 * functions.
 */

#include <stdint.h>
#include <pthread.h>
#include <sched.h>

#include <cassert>
#include <string>
#include <map>
#include <vector>

#include <google/sparse_hash_map>

#include "shortstring.h"
#include "atomic.h"
#include "dirent.h"
#include "catalog_mgr.h"
#include "util.h"

#ifndef CVMFS_GLUE_BUFFER_H_
#define CVMFS_GLUE_BUFFER_H_

namespace glue {

struct Dirent {
  Dirent() { parent_inode = 0; }
  Dirent(const uint64_t p, const NameString &n) { 
    parent_inode = p; 
    name = n;
  }
  uint64_t parent_inode;
  NameString name;
};


class InodeContainer {
 public:
  InodeContainer() {
    inode2path_.set_deleted_key(0);
  }
  void Add(const uint64_t inode, const uint64_t parent_inode, 
           const NameString &name) 
  {
    inode2path_[inode] = Dirent(parent_inode, name);
  }
  void AddDirent(const catalog::DirectoryEntry &dirent) {
    Add(dirent.inode(), dirent.parent_inode(), dirent.name());
  }
  
  bool ConstructPath(const uint64_t inode, PathString *path);
  bool ConstructChain(const uint64_t inode, 
                      std::vector<glue::Dirent> *chain);
  bool Contains(const uint64_t inode) {
    return inode2path_.find(inode) != inode2path_.end();
  }
  
 private:
  typedef google::sparse_hash_map<uint64_t, glue::Dirent, 
          hash_murmur<uint64_t> >
          InodeMap;
  InodeMap inode2path_;
};
  
  
class LookupTracker;
class CwdTracker;  
class OpenTracker;
class Ensemble {
 public: 
  Ensemble(LookupTracker *l, CwdTracker *c, OpenTracker *o) {
    version_ = kVersion;
    Embrace(l, c, o);
  }
  Ensemble(const Ensemble &other);
  Ensemble &operator= (const Ensemble &other);
  ~Ensemble() { Cleanup(); }
  
  bool Find(const uint64_t inode, PathString *path);
  
  inline LookupTracker *lookup_tracker() { return lookup_tracker_; }
  inline CwdTracker *cwd_tracker() { return cwd_tracker_; }
  inline OpenTracker *open_tracker() { return open_tracker_; }
 private:
  static const unsigned kVersion = 1;
  void Embrace(LookupTracker *l, CwdTracker *c, OpenTracker *o);
  void CopyFrom(const Ensemble &other);
  void Cleanup();

  unsigned version_;
  LookupTracker *lookup_tracker_;
  CwdTracker *cwd_tracker_;
  OpenTracker *open_tracker_;
};
  

class LookupTracker {
 public:
  struct Statistics {
    Statistics() {
      atomic_init64(&num_ancient_hits);
      atomic_init64(&num_ancient_misses);
      atomic_init64(&num_busywait_cycles);
      atomic_init64(&num_jmpcwd_hits);
      atomic_init64(&num_jmpcwd_misses);
      atomic_init64(&num_jmpai_hits);
      atomic_init64(&num_jmpai_misses);
    }
    std::string Print() {
      return 
      "hits: " + StringifyInt(atomic_read64(&num_ancient_hits)) +
      "  misses: " + StringifyInt(atomic_read64(&num_ancient_misses)) +
      "  cwd-jmp(hits): " + StringifyInt(atomic_read64(&num_jmpcwd_hits)) +
      "  cwd-jmp(misses): " + StringifyInt(atomic_read64(&num_jmpcwd_misses)) +
      "  ai-jmp(hits): " + StringifyInt(atomic_read64(&num_jmpai_hits)) +
      "  ai-jmp(misses): " + StringifyInt(atomic_read64(&num_jmpai_misses)) +
      "  busy-waits: " + StringifyInt(atomic_read64(&num_busywait_cycles));
    }
    atomic_int64 num_ancient_hits;
    atomic_int64 num_ancient_misses;
    atomic_int64 num_busywait_cycles;
    atomic_int64 num_jmpcwd_hits;
    atomic_int64 num_jmpcwd_misses;
    atomic_int64 num_jmpai_hits;
    atomic_int64 num_jmpai_misses;
  };
  uint64_t GetNumInserts() { return atomic_read64(&buffer_pos_); }
  unsigned GetNumEntries() { return size_; }
  unsigned GetNumBytes() { return size_*sizeof(BufferEntry); }
  Statistics GetStatistics() { return statistics_; }
  
  LookupTracker(const unsigned size);
  LookupTracker(const LookupTracker &other);
  LookupTracker &operator= (const LookupTracker &other);
  ~LookupTracker();
  void Resize(const unsigned new_size);
  void SetEnsemble(Ensemble *ensemble) {
    assert(ensemble->lookup_tracker() == this);
    ensemble_ = ensemble;
  }
  
  inline void Add(const uint64_t inode, const uint64_t parent_inode, 
                  const NameString &name)
  {
    //assert((parent_inode == 0) || ((parent_inode & ((1<<26)-1)) != 0));
    ReadLock();
    
    uint32_t pos = atomic_xadd64(&buffer_pos_, 1) % size_;
    while (!atomic_cas32(&buffer_[pos].busy_flag, 0, 1)) {
      atomic_inc64(&statistics_.num_busywait_cycles);
      sched_yield();
    }
    buffer_[pos].inode = inode;
    buffer_[pos].parent_inode = parent_inode;
    buffer_[pos].name = name;
    atomic_dec32(&buffer_[pos].busy_flag);
    
    Unlock();
  }
  
  inline void AddDirent(const catalog::DirectoryEntry &dirent) {
    Add(dirent.inode(), dirent.parent_inode(), dirent.name());
  }
  bool Find(const uint64_t inode, PathString *path);
  
 private:
  static const unsigned kVersion = 1;
  struct BufferEntry {
    BufferEntry() {
      atomic_init32(&busy_flag);
      inode = parent_inode = 0;
    }
    uint64_t inode;
    uint64_t parent_inode;
    NameString name;
    atomic_int32 busy_flag;
  };
  
  void InitLock();
  void CopyFrom(const LookupTracker &other);
  inline void ReadLock() const {
    int retval = pthread_rwlock_rdlock(rwlock_);
    assert(retval == 0);
  }
  inline void WriteLock() const {
    int retval = pthread_rwlock_wrlock(rwlock_);
    assert(retval == 0);
  }
  inline void Unlock() const {
    int retval = pthread_rwlock_unlock(rwlock_);
    assert(retval == 0);
  }
  bool ConstructPath(const unsigned buffer_idx, PathString *path);
  
  pthread_rwlock_t *rwlock_;
  BufferEntry *buffer_;
  atomic_int64 buffer_pos_;
  unsigned size_;
  unsigned version_;
  Ensemble *ensemble_;
  Statistics statistics_; 
};


/**
 * Saves the inodes of current working directories on this Fuse volume.
 * Required for catalog reloads and reloads of the Fuse module.
 */
class CwdTracker {
public:
  struct Statistics {
    Statistics() {
      atomic_init64(&num_inserts);
      atomic_init64(&num_removes);
      atomic_init64(&num_ancient_hits);
      atomic_init64(&num_ancient_misses);
    }
    std::string Print() {
      return 
      "inserts: " + StringifyInt(atomic_read64(&num_inserts)) +
      "  removes: " + StringifyInt(atomic_read64(&num_removes)) +
      "  ancient(hits): " + StringifyInt(atomic_read64(&num_ancient_hits)) +
      "  ancient(misses): " + StringifyInt(atomic_read64(&num_ancient_misses));
    }
    atomic_int64 num_inserts;
    atomic_int64 num_removes;
    atomic_int64 num_ancient_hits;
    atomic_int64 num_ancient_misses;
  };
  Statistics GetStatistics() { return statistics_; }
  
  explicit CwdTracker(const std::string &mountpoint);
  explicit CwdTracker(const CwdTracker &other);
  CwdTracker &operator= (const CwdTracker &other);
  ~CwdTracker();
  void SetEnsemble(Ensemble *ensemble) {
    assert(ensemble->cwd_tracker() == this);
    ensemble_ = ensemble;
  }
  
  //void Add(const uint64_t inode, const PathString &path);
  bool Find(const uint64_t inode, PathString *path);

  void MaterializePaths(catalog::AbstractCatalogManager *source);
  
private: 
  static const unsigned kVersion = 1;
  
  std::vector<PathString> GatherCwds();
  void InitLock();
  void CopyFrom(const CwdTracker &other);
  inline void Lock() const {
    int retval = pthread_mutex_lock(lock_);
    assert(retval == 0);
  }
  inline void Unlock() const {
    int retval = pthread_mutex_unlock(lock_);
    assert(retval == 0);
  }
  
  pthread_mutex_t *lock_;
  unsigned version_; 
  InodeContainer inode2cwd_;
  std::string mountpoint_;
  Ensemble *ensemble_;
  Statistics statistics_;
};


/**
 * Stores reference counters to active inodes (open directories and 
 * directories of open files).
 * At a certain point in time (before reloads), the set of active inodes can be 
 * transformed into an inode --> path map by MaterializePaths().
 */
class OpenTracker {
 public:
  struct Statistics {
    Statistics() {
      atomic_init64(&num_inserts);
      atomic_init64(&num_removes);
      atomic_init64(&num_references);
      atomic_init64(&num_ancient_hits);
      atomic_init64(&num_ancient_misses);
      atomic_init64(&num_dirent_lookups);
    }
    std::string Print() {
      return 
      "inserts: " + StringifyInt(atomic_read64(&num_inserts)) +
      "  removes: " + StringifyInt(atomic_read64(&num_removes)) +
      "  references: " + StringifyInt(atomic_read64(&num_references)) +
      "  dirent-lookups: " + StringifyInt(atomic_read64(&num_dirent_lookups)) +
      "  ancient(hits): " + StringifyInt(atomic_read64(&num_ancient_hits)) +
      "  ancient(misses): " + StringifyInt(atomic_read64(&num_ancient_misses));
    }
    atomic_int64 num_inserts;
    atomic_int64 num_removes;
    atomic_int64 num_references;
    atomic_int64 num_ancient_hits;
    atomic_int64 num_ancient_misses;
    atomic_int64 num_dirent_lookups;
  };
  Statistics GetStatistics() { return statistics_; }
  
  OpenTracker();
  explicit OpenTracker(const OpenTracker &other);
  OpenTracker &operator= (const OpenTracker &other);
  ~OpenTracker();
  void SetEnsemble(Ensemble *ensemble) {
    assert(ensemble->open_tracker() == this);
    ensemble_ = ensemble;
  }
  
  // TODO: split into VfsGetLiving, VfsGetDeprecated
  void VfsGetLiving(const uint64_t inode);
  void VfsGetDeprecated(const uint64_t inode);
  void VfsPut(const uint64_t inode);
  void MaterializePaths(catalog::AbstractCatalogManager *source);
  bool Find(const uint64_t inode, PathString *path);
  
private:
  static const unsigned kVersion = 1;
  typedef google::sparse_hash_map<uint64_t, uint32_t, hash_murmur<uint64_t> >
          InodeReferences;
  
  void InitLocks();
  void InitSpecialInodes();
  void CopyFrom(const OpenTracker &other);
  inline void LockInodes() const {
    int retval = pthread_mutex_lock(lock_inodes_);
    assert(retval == 0);
  }
  inline void UnlockInodes() const {
    int retval = pthread_mutex_unlock(lock_inodes_);
    assert(retval == 0);
  }
  inline void LockPaths() const {
    int retval = pthread_mutex_lock(lock_paths_);
    assert(retval == 0);
  }
  inline void UnlockPaths() const {
    int retval = pthread_mutex_unlock(lock_paths_);
    assert(retval == 0);
  }
  
  unsigned version_; 
  pthread_mutex_t *lock_inodes_;
  pthread_mutex_t *lock_paths_;
  InodeContainer *inode2path_;
  InodeReferences inode_references_;
  Ensemble *ensemble_;
  Statistics statistics_;
};


class RemountListener : public catalog::RemountListener {
public:
  RemountListener(Ensemble *ensemble) {
    ensemble_ = ensemble;
  }
  void BeforeRemount(catalog::AbstractCatalogManager *source) {
    ensemble_->cwd_tracker()->MaterializePaths(source);
    ensemble_->open_tracker()->MaterializePaths(source);
  }
private:
  Ensemble *ensemble_;
};


}  // namespace glue


//AncientInode2Path(GlueBuffer *glue_buffer, CwdBuffer *cwd_buffer,
//                  ActiveInodesBuffer *active_inodes);


//class CwdBuffer;
//class ActiveInodesBuffer;
/* The snapshot glue buffer is a data structure providing fast writes and
 * slow reads.  It is used to connect to consequtively loaded file system
 * snapshots: the previous snapshots' inodes are stored in the glue buffer
 * and a full path can be constructed from the data in the glue buffer.  This
 * full path can be looked up in the new file system snapshot.
 *
 * Note: the read-write lock has reversed meaning.  Many concurrent writers are
 * allowed but only serialized reading is permitted.
 */
/*class GlueBuffer {
 public:
  struct Statistics {
    Statistics() {
      atomic_init64(&num_ancient_hits);
      atomic_init64(&num_ancient_misses);
      atomic_init64(&num_busywait_cycles);
      atomic_init64(&num_jmpcwd_hits);
      atomic_init64(&num_jmpcwd_misses);
      atomic_init64(&num_jmpai_hits);
      atomic_init64(&num_jmpai_misses);
    }
    std::string Print() {
      return 
        "hits: " + StringifyInt(atomic_read64(&num_ancient_hits)) +
        "  misses: " + StringifyInt(atomic_read64(&num_ancient_misses)) +
        "  cwd-jmp(hits): " + StringifyInt(atomic_read64(&num_jmpcwd_hits)) +
        "  cwd-jmp(misses): " + StringifyInt(atomic_read64(&num_jmpcwd_misses)) +
        "  ai-jmp(hits): " + StringifyInt(atomic_read64(&num_jmpai_hits)) +
        "  ai-jmp(misses): " + StringifyInt(atomic_read64(&num_jmpai_misses)) +
        "  busy-waits: " + StringifyInt(atomic_read64(&num_busywait_cycles));
    }
    atomic_int64 num_ancient_hits;
    atomic_int64 num_ancient_misses;
    atomic_int64 num_busywait_cycles;
    atomic_int64 num_jmpcwd_hits;
    atomic_int64 num_jmpcwd_misses;
    atomic_int64 num_jmpai_hits;
    atomic_int64 num_jmpai_misses;
  };
  uint64_t GetNumInserts() { return atomic_read64(&buffer_pos_); }
  unsigned GetNumEntries() { return size_; }
  unsigned GetNumBytes() { return size_*sizeof(BufferEntry); }
  Statistics GetStatistics() { return statistics_; }
  
  GlueBuffer(const unsigned size);
  GlueBuffer(const GlueBuffer &other);
  GlueBuffer &operator= (const GlueBuffer &other);
  ~GlueBuffer();
  void Resize(const unsigned new_size);
  void SetCwdBuffer(CwdBuffer *cwd_buffer) { cwd_buffer_ = cwd_buffer; }
  void SetActiveInodesBuffer(ActiveInodesBuffer *active_inodes) { 
    active_inodes_ = active_inodes; 
  }
  
  inline void Add(const uint64_t inode, const uint64_t parent_inode, 
                  const uint32_t generation, const NameString &name)
  {
    //assert((parent_inode == 0) || ((parent_inode & ((1<<26)-1)) != 0));
    ReadLock();
    
    uint32_t pos = atomic_xadd64(&buffer_pos_, 1) % size_;
    while (!atomic_cas32(&buffer_[pos].busy_flag, 0, 1)) {
      atomic_inc64(&statistics_.num_busywait_cycles);
      sched_yield();
    }
    buffer_[pos].generation = generation;
    buffer_[pos].inode = inode;
    buffer_[pos].parent_inode = parent_inode;
    buffer_[pos].name = name;
    atomic_dec32(&buffer_[pos].busy_flag);

    Unlock();
  }
  
  inline void AddDirent(const catalog::DirectoryEntry &dirent) {
    Add(dirent.inode(), dirent.parent_inode(), dirent.generation(), 
        dirent.name());
  }
  
  bool Find(const uint64_t inode,  const uint32_t current_generation,
            PathString *path);
  
 private:
  static const unsigned kVersion = 1;
  struct BufferEntry {
    BufferEntry() {
      atomic_init32(&busy_flag);
      generation = 0;
      inode = parent_inode = 0;
    }
    uint32_t generation;
    uint64_t inode;
    uint64_t parent_inode;
    NameString name;
    atomic_int32 busy_flag;
  };
  
  void InitLock();
  void CopyFrom(const GlueBuffer &other);
  inline void ReadLock() const {
    int retval = pthread_rwlock_rdlock(rwlock_);
    assert(retval == 0);
  }
  inline void WriteLock() const {
    int retval = pthread_rwlock_wrlock(rwlock_);
    assert(retval == 0);
  }
  inline void Unlock() const {
    int retval = pthread_rwlock_unlock(rwlock_);
    assert(retval == 0);
  }
  bool ConstructPath(const unsigned buffer_idx, PathString *path);
  
  pthread_rwlock_t *rwlock_;
  BufferEntry *buffer_;
  atomic_int64 buffer_pos_;
  unsigned size_;
  unsigned version_;
  // If reconstructing fails, there might be a hit in the cwd buffer
  CwdBuffer *cwd_buffer_;
  ActiveInodesBuffer *active_inodes_;
  Statistics statistics_;
};*/


/**
 * Saves the inodes of current working directories on this Fuse volume.
 * Required for catalog reloads and reloads of the Fuse module.
 */
/*class CwdBuffer {
 public:
  struct Statistics {
    Statistics() {
      atomic_init64(&num_inserts);
      atomic_init64(&num_removes);
      atomic_init64(&num_ancient_hits);
      atomic_init64(&num_ancient_misses);
    }
    std::string Print() {
      return 
        "inserts: " + StringifyInt(atomic_read64(&num_inserts)) +
        "  removes: " + StringifyInt(atomic_read64(&num_removes)) +
        "  ancient(hits): " + StringifyInt(atomic_read64(&num_ancient_hits)) +
        "  ancient(misses): " + StringifyInt(atomic_read64(&num_ancient_misses));
    }
    atomic_int64 num_inserts;
    atomic_int64 num_removes;
    atomic_int64 num_ancient_hits;
    atomic_int64 num_ancient_misses;
  };
  Statistics GetStatistics() { return statistics_; }
  
  explicit CwdBuffer(const std::string &mountpoint);
  explicit CwdBuffer(const CwdBuffer &other);
  CwdBuffer &operator= (const CwdBuffer &other);
  ~CwdBuffer();
  
  std::vector<PathString> GatherCwds();
  void Add(const uint64_t inode, const PathString &path);
  void Remove(const uint64_t);
  bool Find(const uint64_t inode, PathString *path);
  
  void BeforeRemount(catalog::AbstractCatalogManager *source);
 
 private: 
  static const unsigned kVersion = 1;
  void InitLock();
  void CopyFrom(const CwdBuffer &other);
  inline void Lock() const {
    int retval = pthread_mutex_lock(lock_);
    assert(retval == 0);
  }
  inline void Unlock() const {
    int retval = pthread_mutex_unlock(lock_);
    assert(retval == 0);
  }
  
  pthread_mutex_t *lock_;
  unsigned version_; 
  std::map<uint64_t, PathString> inode2cwd_;
  std::string mountpoint_;
  Statistics statistics_;
};*/


/**
 * Stores reference counters to active inodes (open directories and 
 * directories of open files).
 * At a certain point in time (before reloads), the set of active inodes can be 
 * transformed into an inode --> path map by MaterializePaths().
 */
/*class ActiveInodesBuffer {
 public:
  struct Statistics {
    Statistics() {
      atomic_init64(&num_inserts);
      atomic_init64(&num_removes);
      atomic_init64(&num_references);
      atomic_init64(&num_ancient_hits);
      atomic_init64(&num_ancient_misses);
      atomic_init64(&num_dirent_lookups);
    }
    std::string Print() {
      return 
      "inserts: " + StringifyInt(atomic_read64(&num_inserts)) +
      "  removes: " + StringifyInt(atomic_read64(&num_removes)) +
      "  references: " + StringifyInt(atomic_read64(&num_references)) +
      "  dirent-lookups: " + StringifyInt(atomic_read64(&num_dirent_lookups)) +
      "  ancient(hits): " + StringifyInt(atomic_read64(&num_ancient_hits)) +
      "  ancient(misses): " + StringifyInt(atomic_read64(&num_ancient_misses));
    }
    atomic_int64 num_inserts;
    atomic_int64 num_removes;
    atomic_int64 num_references;
    atomic_int64 num_ancient_hits;
    atomic_int64 num_ancient_misses;
    atomic_int64 num_dirent_lookups;
  };
  Statistics GetStatistics() { return statistics_; }
  
  ActiveInodesBuffer();
  explicit ActiveInodesBuffer(const ActiveInodesBuffer &other);
  ActiveInodesBuffer &operator= (const ActiveInodesBuffer &other);
  ~ActiveInodesBuffer();
  
  // TODO: split into VfsGetLiving, VfsGetDeprecated
  void VfsGet(const uint64_t inode);
  void VfsPut(const uint64_t inode);
  void MaterializePaths(catalog::AbstractCatalogManager *source);
  bool Find(const uint64_t inode, PathString *path);
  
 private:
  static const unsigned kVersion = 1;
  struct MinimalDirent {
    MinimalDirent() { parent_inode = 0; }
    MinimalDirent(const uint64_t p, const NameString &n) { 
      parent_inode = p; 
      name = n;
    }
    uint64_t parent_inode;
    NameString name;
  };
  typedef google::sparse_hash_map<uint64_t, uint32_t, hash_murmur<uint64_t> >
          InodeReferences;
  typedef google::sparse_hash_map<uint64_t, MinimalDirent, 
                                  hash_murmur<uint64_t> >
          PathMap;
  
  void InitLocks();
  void InitSpecialInodes();
  void CopyFrom(const ActiveInodesBuffer &other);
  inline void LockInodes() const {
    int retval = pthread_mutex_lock(lock_inodes_);
    assert(retval == 0);
  }
  inline void UnlockInodes() const {
    int retval = pthread_mutex_unlock(lock_inodes_);
    assert(retval == 0);
  }
  inline void LockPaths() const {
    int retval = pthread_mutex_lock(lock_paths_);
    assert(retval == 0);
  }
  inline void UnlockPaths() const {
    int retval = pthread_mutex_unlock(lock_paths_);
    assert(retval == 0);
  }
  bool ConstructPath(const uint64_t inode, PathString *path);
  
  unsigned version_; 
  pthread_mutex_t *lock_inodes_;
  pthread_mutex_t *lock_paths_;
  PathMap *inode2path_;
  InodeReferences inode_references_;
  Statistics statistics_;
};*/

#endif  // CVMFS_GLUE_BUFFER_H_
