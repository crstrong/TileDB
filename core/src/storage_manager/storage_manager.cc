/**
 * @file   storage_manager.cc
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2017 TileDB, Inc.
 * @copyright Copyright (c) 2016 MIT and Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 * This file implements the StorageManager class.
 */

#include <algorithm>

#include "logger.h"
#include "storage_manager.h"
#include "utils.h"

namespace tiledb {

/* ****************************** */
/*   CONSTRUCTORS & DESTRUCTORS   */
/* ****************************** */

StorageManager::StorageManager() {
  async_done_ = false;
  vfs_ = nullptr;
}

StorageManager::~StorageManager() {
  async_stop();
  delete async_thread_[0];
  delete async_thread_[1];
  delete vfs_;
}

/* ****************************** */
/*               API              */
/* ****************************** */

bool StorageManager::fragment_exists(const URI& fragment_uri) {
  return vfs_->is_dir(fragment_uri);
}

Status StorageManager::fragment_rename(const URI& fragment_uri) {
/* TODO
  std::string fragment_path = uri.to_posix_path();
  std::string parent_dir = utils::parent_path(fragment_path);
  std::string new_fragment_name =
      parent_dir + "/" + fragment_path.substr(parent_dir.size() + 2);
  // move the fragment directory
  RETURN_NOT_OK(vfs::move(fragment_path, new_fragment_name));
  // create a new fragment file in the new directory
  RETURN_NOT_OK(vfs::create_fragment_file(new_fragment_name));
*/

  return Status::Ok();
}



Status StorageManager::array_create(ArraySchema* array_schema) const {
  // Check array schema
  if (array_schema == nullptr) {
    return LOG_STATUS(
        Status::StorageManagerError("Cannot create array; Empty array schema"));
  }

  // Create array directory
  const URI& array_uri = array_schema->array_uri();
  RETURN_NOT_OK(vfs_->create_dir(array_uri));

  // Store array schema
  RETURN_NOT_OK(array_schema->store(array_uri));

  // Create array filelock
  URI filelock_uri = array_uri.join_path(constants::array_filelock_name);
  RETURN_NOT_OK(vfs_->create_file(filelock_uri));

  // Success
  return Status::Ok();
}

Status StorageManager::async_push_query(Query* query, int i) {
  // Set the request status
  query->set_status(QueryStatus::INPROGRESS);

  // Push request
  {
    std::lock_guard<std::mutex> lock(async_mtx_[i]);
    async_queue_[i].emplace(query);
  }

  // Signal AIO thread
  async_cv_[i].notify_one();

  return Status::Ok();
}

Status StorageManager::group_create(const URI& group) const {
  // Create group directory
  RETURN_NOT_OK(vfs_->create_dir(group));

  // Create group file
  URI group_filename = group.join_path(constants::group_filename);
  RETURN_NOT_OK(vfs_->create_file(group_filename));

  return Status::Ok();
}

Status StorageManager::init() {
  // Create a thread to handle the user asynchronous I/O
  async_thread_[0] = new std::thread(async_start, this, 0);

  // Create a thread to handle the internal asynchronous I/O
  async_thread_[1] = new std::thread(async_start, this, 1);

  // Create virtual filesystem manager
  vfs_ = new VFS();

  return Status::Ok();
}

Status StorageManager::query_init(
    Query* query,
    const char* array_name,
    QueryMode mode,
    const void* subarray,
    const char** attributes,
    int attribute_num,
    void** buffers,
    uint64_t* buffer_sizes) {
  // Open the array
  std::vector<FragmentMetadata*> fragment_metadata;
  ArraySchema* array_schema;
  RETURN_NOT_OK(array_open(
      URI(array_name), mode, subarray, &array_schema, &fragment_metadata));

  // Initialize query
  return query->init(
      this,
      array_schema,
      fragment_metadata,
      mode,
      subarray,
      attributes,
      attribute_num,
      buffers,
      buffer_sizes);
}

Status StorageManager::query_submit(Query* query) {
  QueryMode mode = query->mode();
  if (is_read_mode(mode))
    return query->read();

  return query->write();
}

Status StorageManager::query_submit_async(
    Query* query, void* (*callback)(void*), void* callback_data) {
  // Push the query into the async queue
  query->set_callback(callback, callback_data);
  return async_push_query(query, 0);
}

/* ****************************** */
/*         PRIVATE METHODS        */
/* ****************************** */

Status StorageManager::array_close(
    const URI& array_uri,
    const std::vector<FragmentMetadata*>& fragment_metadata) {
  // Lock mutex
  open_array_mtx_.lock();

  // Find the open array entry
  std::map<std::string, OpenArray*>::iterator it =
      open_arrays_.find(array_uri.to_string());

  // Sanity check
  if (it == open_arrays_.end()) {
    return LOG_STATUS(Status::StorageManagerError(
        "Cannot close array; Open array entry not found"));
  }

  // For easy reference
  OpenArray* open_array = it->second;

  // Lock the mutex of the array
  open_array->mtx_lock();

  // Decrement counter
  open_array->decr_cnt();

  // Remove fragment metadata
  for (auto& metadata : fragment_metadata)
    open_array->fragment_metadata_rm(metadata->fragment_uri());

  // Potentially remove open array entry
  Status st = Status::Ok();
  if (open_array->cnt() == 0) {
    // TODO: we may want to leave this to a cache manager
    st = vfs_->filelock_unlock(
        array_uri.join_path(constants::array_filelock_name),
        open_array->filelock_fd());
    open_array->mtx_unlock();
    delete open_array;
    open_arrays_.erase(it);
  } else {
    // Unlock the mutex of the array
    open_array->mtx_unlock();
  }

  // Unlock mutex
  open_array_mtx_.unlock();

  return st;
}

Status StorageManager::array_open(
    const URI& array_uri,
    QueryMode mode,
    const void* subarray,
    const ArraySchema** array_schema,
    std::vector<FragmentMetadata*>* fragment_metadata) {
  // Get the open array entry
  OpenArray* open_array;
  RETURN_NOT_OK(open_array_get_entry(array_uri, &open_array));

  // Lock the mutex of the array
  open_array->mtx_lock();

  // Increment counter
  open_array->incr_cnt();

  // Lock the filelock of the array
  if (!open_array->filelocked()) {
    int fd;
    RETURN_NOT_OK_ELSE(
        vfs_->filelock_lock(
            array_uri.join_path(constants::array_filelock_name), &fd, true),
        array_open_error(open_array, *fragment_metadata));
    open_array->set_filelock_fd(fd);
  }

  // Load array schema
  RETURN_NOT_OK_ELSE(
      open_array_load_schema(array_uri, open_array),
      array_open_error(open_array, *fragment_metadata));
  *array_schema = open_array->array_schema();

  // Get fragment metadata only in read mode
  if (is_read_mode(mode))
    RETURN_NOT_OK_ELSE(
        open_array_load_fragment_metadata(
            open_array, subarray, fragment_metadata),
        array_open_error(open_array, *fragment_metadata));

  // Unlock the array mutex
  open_array->mtx_unlock();

  return Status::Ok();
}

Status StorageManager::array_open_error(
    OpenArray* open_array,
    const std::vector<FragmentMetadata*>& fragment_metadata) {
  open_array->mtx_unlock();
  return array_close(open_array->array_uri(), fragment_metadata);
}

void StorageManager::async_start(StorageManager* storage_manager, int i) {
  storage_manager->async_process_queries(i);
}

void StorageManager::async_stop() {
  async_done_ = true;
  async_cv_[0].notify_one();
  async_cv_[1].notify_one();
  async_thread_[0]->join();
  async_thread_[1]->join();
}

void StorageManager::async_process_query(Query* query) {
  // For easy reference
  Status st = query->async_process();
  if (!st.ok())
    LOG_STATUS(st);
}

void StorageManager::async_process_queries(int i) {
  while (!async_done_) {
    std::unique_lock<std::mutex> lock(async_mtx_[i]);
    async_cv_[i].wait(lock, [this, i] {
      return (async_queue_[i].size() > 0) || async_done_;
    });
    if (async_done_)
      break;
    auto query = async_queue_[i].front();
    async_queue_[i].pop();
    lock.unlock();
    async_process_query(query);
  }
}

Status StorageManager::open_array_get_entry(
    const URI& array_uri, OpenArray** open_array) {
  // Lock mutex
  open_array_mtx_.lock();

  // Find the open array entry
  std::map<std::string, OpenArray*>::iterator it =
      open_arrays_.find(array_uri.to_string());
  // Create and init entry if it does not exist
  if (it == open_arrays_.end()) {
    *open_array = new OpenArray();
    open_arrays_[array_uri.to_string()] = *open_array;
  } else {
    *open_array = it->second;
  }

  // Unlock mutex
  open_array_mtx_.unlock();

  return Status::Ok();
}

Status StorageManager::open_array_load_schema(
    const URI& array_uri, OpenArray* open_array) {
  // Do nothing if the array schema is already loaded
  if (open_array->array_schema() != nullptr)
    return Status::Ok();

  ArraySchema* array_schema = new ArraySchema();
  RETURN_NOT_OK_ELSE(array_schema->load(array_uri), delete array_schema);
  open_array->set_array_schema(array_schema);

  return Status::Ok();
}

Status StorageManager::open_array_load_fragment_metadata(
    OpenArray* open_array,
    const void* subarray,
    std::vector<FragmentMetadata*>* fragment_metadata) {
  // Get all the fragment uris, sorted by timestamp
  std::vector<URI> fragment_uris;
  const URI& array_uri = open_array->array_uri();
  RETURN_NOT_OK(get_fragment_uris(array_uri, subarray, &fragment_uris));
  sort_fragment_uris(&fragment_uris);

  if (fragment_uris.size() == 0)
    return Status::Ok();

  // Load the metadata for each fragment
  FragmentMetadata* metadata;
  for (auto& uri : fragment_uris) {
    // Find metadata entry in open array
    metadata = open_array->fragment_metadata_get(uri);

    // If not found, load metadata and store in open array
    if (metadata == nullptr) {
      bool dense = !vfs_->is_file(uri.join_path(
          std::string("/") + constants::coords + constants::file_suffix));
      metadata = new FragmentMetadata(open_array->array_schema(), dense, uri);
      RETURN_NOT_OK_ELSE(metadata->load(), delete metadata);
      open_array->fragment_metadata_add(metadata);
    }

    // Add to list
    fragment_metadata->push_back(metadata);
  }

  return Status::Ok();
}

Status StorageManager::get_fragment_uris(
    const URI& array_uri,
    const void* subarray,
    std::vector<URI>* fragment_uris) const {
  // Get all uris in the array directory
  std::vector<URI> uris;
  RETURN_NOT_OK(vfs_->ls(array_uri, &uris));

  // Get only the fragment uris
  for (auto& uri : uris) {
    // TODO: check here if the fragment overlaps subarray
    if (vfs_->is_file(uri.join_path(constants::fragment_filename)))
      fragment_uris->push_back(uri);
  }

  return Status::Ok();
}

void StorageManager::sort_fragment_uris(std::vector<URI>* fragment_uris) const {
  // Do nothing if there are not enough fragments
  uint64_t fragment_num = fragment_uris->size();
  if (fragment_num <= 0)
    return;

  // Initializations
  std::string t_str;
  uint64_t t, pos = 0;
  std::vector<std::pair<uint64_t, uint64_t>> t_pos_vec;

  // Get the timestamp for each fragment
  for (auto& uri : *fragment_uris) {
    // Get fragment name
    std::string fragment_name = uri.last_path_part();
    assert(utils::starts_with(fragment_name, "__"));

    // Get timestamp in the end of the name after '_'
    assert(fragment_name.find("_") != std::string::npos);
    t_str = fragment_name.substr(fragment_name.find("_") + 1);
    sscanf(t_str.c_str(), "%lld", (long long int*)&t);
    t_pos_vec.push_back(std::pair<uint64_t, uint64_t>(t, pos++));
  }

  // Sort the names based on the timestamps
  std::sort(t_pos_vec.begin(), t_pos_vec.end());
  std::vector<URI> fragment_uris_sorted;
  fragment_uris_sorted.resize(fragment_num);
  for (uint64_t i = 0; i < fragment_num; ++i)
    fragment_uris_sorted[i] = (*fragment_uris)[t_pos_vec[i].second];
  *fragment_uris = fragment_uris_sorted;
}

}  // namespace tiledb

/*
// TODO: Object types should be Enums
int StorageManager::dir_type(const char* dir) {
  std::string dir_real = vfs::real_dir(dir);

  if (utils::is_group(dir_real))
    return TILEDB_GROUP;

  if (utils::is_array(dir_real))
    return TILEDB_ARRAY;

  return -1;
}

Status StorageManager::consolidation_finalize(
    Fragment* new_fragment, const std::vector<URI>& old_fragments) {
  // Trivial case - there was no consolidation
  if (old_fragments.size() == 0)
    return Status::Ok();

  // Acquire exclusive lock on consolidation filelock
  int fd;
  Status st;
  st = consolidation_lock(
      new_fragment->array()->array_schema()->array_uri().to_posix_path(),
      &fd,
      false);
  if (!st.ok()) {
    delete new_fragment;
    return st;
  }

  // Finalize new fragment - makes the new fragment visible to new reads
  st = new_fragment->finalize();
  delete new_fragment;
  if (!st.ok()) {
    return st;
  }

  // Make old fragments invisible to new reads
  int fragment_num = old_fragments.size();
  for (int i = 0; i < fragment_num; ++i) {
    // Delete special fragment file inside the fragment directory
    std::string old_fragment_filename =
        old_fragments[i]
            .join_path(constants::fragment_filename)
            .to_posix_path();
    RETURN_NOT_OK(vfs::delete_file(old_fragment_filename));
  }

  // Unlock consolidation filelock
  RETURN_NOT_OK(consolidation_unlock(fd));

  // Delete old fragments
  for (int i = 0; i < fragment_num; ++i) {
    RETURN_NOT_OK(vfs::delete_dir(old_fragments[i]));
  }
  return Status::Ok();
}

Status StorageManager::group_clear(const URI& group) const {
  URI group_uri = vfs::abs_path(group);

  // Check if group exists
  if (!utils::is_group(group_uri)) {
    return LOG_STATUS(Status::StorageManagerError(
        std::string("Group '") + group_uri.to_string() + "' does not exist"));
  }

  // Delete all groups, arrays inside the group directory
  // TODO: this functionalty should be moved to a Filesystem IO backend
  std::vector<std::string> paths;
  RETURN_NOT_OK(vfs::ls(group_uri.to_string(), &paths));
  for (auto& path : paths) {
    if (utils::is_group(path)) {  // Group
      RETURN_NOT_OK(group_delete(path))
    } else if (utils::is_array(path)) {  // Array
      RETURN_NOT_OK(array_delete(path))
    } else {  // Non TileDB related
      return LOG_STATUS(Status::StorageManagerError(
          std::string("Cannot delete non TileDB related element '") + path +
          "'"));
    }
  }
  return Status::Ok();
}

Status StorageManager::group_delete(const URI& group) const {
  // Clear the group
  RETURN_NOT_OK(group_clear(group));

  // Delete group directory
  RETURN_NOT_OK(vfs::delete_dir(group));

  return Status::Ok();
}

Status StorageManager::group_move(
    const URI& old_group, const URI& new_group) const {
  // Get real group directory names
  URI old_group_uri = vfs::abs_path(old_group);
  URI new_group_uri = vfs::abs_path(new_group);

  // Check if the old group exists
  if (!utils::is_group(old_group_uri)) {
    return LOG_STATUS(Status::StorageManagerError(
        std::string("Group '") + old_group_uri.to_string() +
        "' does not exist"));
  }

  // Make sure that the new group is not an existing directory
  if (vfs::is_dir(new_group_uri)) {
    return LOG_STATUS(Status::StorageManagerError(
        std::string("Group '") + new_group_uri.to_string() +
        "' already exists"));
  }

  return vfs::move(old_group_uri, new_group_uri);
}

Status StorageManager::array_delete(const URI& array) const {
  RETURN_NOT_OK(array_clear(array));
  // Delete array directory
  RETURN_NOT_OK(vfs::delete_dir(array));
  return Status::Ok();
}

Status StorageManager::array_move(
    const URI& old_array, const URI& new_array) const {
  URI old_array_uri = vfs::abs_path(old_array);
  URI new_array_uri = vfs::abs_path(new_array);

  // Check if the old array exists
  if (!utils::is_array(old_array_uri)) {
    return LOG_STATUS(Status::StorageManagerError(
        std::string("Array '") + old_array_uri.to_string() +
        "' does not exist"));
  }

  // Make sure that the new array is not an existing directory
  if (vfs::is_dir(new_array_uri)) {
    return LOG_STATUS(Status::StorageManagerError(
        std::string("Array '") + new_array_uri.to_string() +
        "' already exists"));
  }
  // Rename array
  RETURN_NOT_OK(vfs::move(old_array_uri, new_array_uri));

  // Incorporate new name in the array schema
  ArraySchema* array_schema = new ArraySchema();
  RETURN_NOT_OK_ELSE(array_schema->load(new_array_uri), delete array_schema);
  array_schema->set_array_uri(new_array_uri);

  // Store the new schema
  RETURN_NOT_OK_ELSE(array_schema->store(new_array_uri), delete array_schema);

  // Clean up
  delete array_schema;

  // Success
  return Status::Ok();
}

Status StorageManager::array_clear(const URI& array) const {
  // Get real array directory name
  URI array_uri = vfs::abs_path(array);

  // Check if the array exists
  if (!utils::is_array(array_uri)) {
    return LOG_STATUS(Status::StorageManagerError(
        std::string("Array '") + array_uri.to_string() + "' does not exist"));
  }

  // Delete the entire array directory except for the array schema file
  std::vector<std::string> paths;
  RETURN_NOT_OK(vfs::ls(array_uri.to_posix_path(), &paths));
  for (auto& path : paths) {
    if (utils::is_array_schema(path) || utils::is_consolidation_lock(path))
      continue;
    if (utils::is_fragment(path)) {  // Fragment
      RETURN_NOT_OK(vfs::delete_dir(path));
    } else {  // Non TileDB related
      return LOG_STATUS(Status::StorageManagerError(
          std::string("Cannot delete non TileDB related path '") + path + "'"));
    }
  }
  return Status::Ok();
}

Status StorageManager::array_finalize(Array* array) {
  // If the array is NULL, do nothing
  if (array == nullptr)
    return Status::Ok();

  // Finalize and close the array
  RETURN_NOT_OK_ELSE(array->finalize(), delete array);
  if (is_read_mode(array->query_->mode()))
    RETURN_NOT_OK_ELSE(
        array_close(array->array_schema()->array_uri()), delete array)

  // Clean up
  delete array;

  return Status::Ok();
}

    Status StorageManager::ls(
        const URI& parent_uri,
        char** object_paths,
        tiledb_object_t* object_types,
        int* object_num) const {
  // Initialize object counter
  int obj_idx = 0;

  std::vector<std::string> paths;
  RETURN_NOT_OK(vfs::ls(parent_uri.to_posix_path(), &paths));

  for (auto& path : paths) {
    if (utils::is_group(path)) {  // Group
      if (obj_idx == *object_num) {
        return LOG_STATUS(Status::StorageManagerError(
            "Cannot list TileDB path; object buffer overflow"));
      }
      strcpy(object_paths[obj_idx], path.c_str());
      object_types[obj_idx] = TILEDB_GROUP;
      ++obj_idx;
    } else if (utils::is_array(path)) {  // Array
      if (obj_idx == *object_num) {
        return LOG_STATUS(Status::StorageManagerError(
            "Cannot list TileDB path; object buffer overflow"));
      }
      strcpy(object_paths[obj_idx], path.c_str());
      object_types[obj_idx] = TILEDB_ARRAY;
      ++obj_idx;
    }
  }
  // Set the number of objects
  *object_num = obj_idx;

  return Status::Ok();
}

Status StorageManager::ls_c(const URI& parent_uri, int* object_num) const {
  *object_num = 0;
  // Initialize directory number
  std::vector<std::string> paths;
  RETURN_NOT_OK(vfs::ls(parent_uri.to_posix_path(), &paths));

  *object_num =
      std::count_if(paths.begin(), paths.end(), [](std::string& path) {
        return utils::is_group(path) || utils::is_array(path);
      });
  return Status::Ok();
}

Status StorageManager::clear(const URI& uri) const {
  if (utils::is_group(uri)) {
    return group_clear(uri);
  } else if (utils::is_array(uri)) {
    return array_clear(uri);
  } else {
    return LOG_STATUS(
        Status::StorageManagerError("Clear failed; Invalid directory"));
  }
}

Status StorageManager::delete_entire(const URI& uri) {
  if (utils::is_group(uri)) {
    return group_delete(uri);
  } else if (utils::is_array(uri)) {
    return array_delete(uri);
  } else {
    return LOG_STATUS(
        Status::StorageManagerError("Delete failed; Invalid directory"));
  }
}

Status StorageManager::move(const URI& old_uri, const URI& new_uri) {
  if (utils::is_group(old_uri)) {
    return group_move(old_uri, new_uri);
  } else if (utils::is_array(old_uri)) {
    return array_move(old_uri, new_uri);
  } else {
    return LOG_STATUS(
        Status::StorageManagerError("Move failed; Invalid source directory"));
  }
}

Status StorageManager::aio_submit(AIORequest* aio_request, int i) {
  aio_push_request(aio_request, i);

  return Status::Ok();
}

Status StorageManager::array_consolidate(const URI& array_uri) {
 // TODO
  // Create an array object
  Array* array;
  RETURN_NOT_OK(
      array_init(array, array_uri, QueryMode::READ, nullptr, nullptr, 0));

  // Consolidate array (TODO: unhandled error handling here)
  Fragment* new_fragment;
  std::vector<URI> old_fragments;
  Status st_array_consolidate =
      array->consolidate(new_fragment, &old_fragments);

  // Close the array
  Status st_array_close = array_close(array->array_schema()->array_uri());

  // Finalize consolidation
  Status st_consolidation_finalize =
      consolidation_finalize(new_fragment, old_fragments);

  // Finalize array
  Status st_array_finalize = array->finalize();
  delete array;
  if (!st_array_finalize.ok()) {
    // TODO: Status:
    return LOG_STATUS(
        Status::StorageManagerError(std::string("Could not finalize array: ")
                                        .append(array_uri.to_string())));
  }
  if (!(st_array_close.ok() && !st_consolidation_finalize.ok()))
    return LOG_STATUS(
        Status::StorageManagerError(std::string("Could not consolidate array: ")
                                        .append(array_uri.to_string())));
  return Status::Ok();
}

 Status Array::consolidate(
    Fragment*& new_fragment, std::vector<uri::URI>* old_fragments) {
  /* TODO

    // Trivial case
    if (fragments_.size() == 1)
      return Status::Ok();

    // Get new fragment name
    std::string new_fragment_name = this->new_fragment_name();
    if (new_fragment_name == "") {
      return LOG_STATUS(Status::ArrayError("Cannot produce new fragment name"));
    }

    // Create new fragment
    new_fragment = new Fragment(this);
    RETURN_NOT_OK(new_fragment->init(new_fragment_name));

    // Consolidate on a per-attribute basis
    Status st;
    for (int i = 0; i < array_schema_->attribute_num() + 1; ++i) {
      st = consolidate(new_fragment, i);
      if (!st.ok()) {
        utils::delete_fragment(new_fragment->fragment_uri());
        delete new_fragment;
        return st;
      }
    }

    // Get old fragment names
    int fragment_num = fragments_.size();
    for (int i = 0; i < fragment_num; ++i)
      old_fragments->push_back(fragments_[i]->fragment_uri());

  return Status::Ok();
}

Status Array::consolidate(Fragment* new_fragment, int attribute_id) {
  TODO
  // For easy reference
  int attribute_num = array_schema_->attribute_num();
  uint64_t consolidation_buffer_size =
      constants::consolidation_buffer_size;

  // Do nothing if the array is dense for the coordinates attribute
  if (array_schema_->dense() && attribute_id == attribute_num)
    return Status::Ok();

  // Prepare buffers
  void** buffers;
  size_t* buffer_sizes;

  // Count the number of variable attributes
  int var_attribute_num = array_schema_->var_attribute_num();

  // Populate the buffers
  int buffer_num = attribute_num + 1 + var_attribute_num;
  buffers = (void**)malloc(buffer_num * sizeof(void*));
  buffer_sizes = (size_t*)malloc(buffer_num * sizeof(size_t));
  int buffer_i = 0;
  for (int i = 0; i < attribute_num + 1; ++i) {
    if (i == attribute_id) {
      buffers[buffer_i] = malloc(consolidation_buffer_size);
      buffer_sizes[buffer_i] = consolidation_buffer_size;
      ++buffer_i;
      if (array_schema_->var_size(i)) {
        buffers[buffer_i] = malloc(consolidation_buffer_size);
        buffer_sizes[buffer_i] = consolidation_buffer_size;
        ++buffer_i;
      }
    } else {
      buffers[buffer_i] = nullptr;
      buffer_sizes[buffer_i] = 0;
      ++buffer_i;
      if (array_schema_->var_size(i)) {
        buffers[buffer_i] = nullptr;
        buffer_sizes[buffer_i] = 0;
        ++buffer_i;
      }
    }
  }

  // Read and write attribute until there is no overflow
  Status st_write;
  Status st_read;
  do {
    // Read
    st_read = read(buffers, buffer_sizes);
    if (!st_read.ok())
      break;

    // Write
    st_write =
        new_fragment->write((const void**)buffers, (const size_t*)buffer_sizes);
    if (!st_write.ok())
      break;
  } while (overflow(attribute_id));

  // Clean up
  for (int i = 0; i < buffer_num; ++i) {
    if (buffers[i] != nullptr)
      free(buffers[i]);
  }
  free(buffers);
  free(buffer_sizes);

  // Error
  if (!st_write.ok()) {
    return st_write;
  }
  if (!st_read.ok()) {
    return st_read;
  }

  return Status::Ok();
}

*/
