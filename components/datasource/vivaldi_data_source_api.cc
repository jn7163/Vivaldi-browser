// Copyright (c) 2017 Vivaldi Technologies AS. All rights reserved

#include "components/datasource/vivaldi_data_source_api.h"

#include "app/vivaldi_constants.cc"
#include "base/containers/flat_set.h"
#include "base/guid.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/lazy_instance.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread_restrictions.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_paths.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/api/bookmarks/bookmarks_private_api.h"
#include "extensions/browser/browser_context_keyed_api_factory.h"
#include "extensions/common/api/extension_types.h"
#include "prefs/vivaldi_gen_prefs.h"
#include "ui/base/models/tree_node_iterator.h"

const char kDatasourceFilemappingFilename[] = "file_mapping.json";
const char kDatasourceFilemappingTmpFilename[] = "file_mapping.tmp";
const base::FilePath::StringPieceType kThumbnailDirectory =
    FILE_PATH_LITERAL("VivaldiThumbnails");

namespace extensions {

// Helper to store ref-counted VivaldiDataSourcesAPI in BrowserContext.
class VivaldiDataSourcesAPIHolder : public BrowserContextKeyedAPI {
 public:
  explicit VivaldiDataSourcesAPIHolder(content::BrowserContext* context);
  ~VivaldiDataSourcesAPIHolder() override;

  // BrowserContextKeyedAPI implementation.
  static BrowserContextKeyedAPIFactory<VivaldiDataSourcesAPIHolder>*
    GetFactoryInstance();

 private:
  friend class VivaldiDataSourcesAPI;

  friend class BrowserContextKeyedAPIFactory<VivaldiDataSourcesAPIHolder>;
  static const char* service_name() {
    return "VivaldiDataSourcesAPI";
  }
  static const bool kServiceIsNULLWhileTesting = false;
  static const bool kServiceRedirectedInIncognito = true;

  // KeyedService
  void Shutdown() override;

  scoped_refptr<VivaldiDataSourcesAPI> api_;
};

const char* VivaldiDataSourcesAPI::kDataMappingPrefs[] = {
    vivaldiprefs::kThemeWindowBackgroundImageUrl,
    vivaldiprefs::kStartpageImagePathCustom,
};

VivaldiDataSourcesAPI::VivaldiDataSourcesAPI(Profile* profile)
    : profile_(profile),
      user_data_dir_(profile->GetPath()),
      sequence_task_runner_(base::CreateSequencedTaskRunner(
          {base::ThreadPool(), base::TaskPriority::USER_VISIBLE,
           base::MayBlock()})) {}

VivaldiDataSourcesAPI::~VivaldiDataSourcesAPI() {}

// static
int VivaldiDataSourcesAPI::FindMappingPreference(base::StringPiece preference) {
  const char* const* i = std::find(std::cbegin(kDataMappingPrefs),
                                   std::cend(kDataMappingPrefs), preference);
  if (i == std::cend(kDataMappingPrefs))
    return -1;
  return static_cast<int>(i - kDataMappingPrefs);
}

// static
bool VivaldiDataSourcesAPI::ReadFileOnBlockingThread(
    const base::FilePath& file_path,
    std::vector<unsigned char>* buffer) {
  base::File file(file_path, base::File::FLAG_READ | base::File::FLAG_OPEN);
  if (!file.IsValid()) {
    // Treat the file that does not exist as an empty file and do not log the
    // error.
    if (file.error_details() != base::File::FILE_ERROR_NOT_FOUND) {
      LOG(ERROR) << "Failed to open file " << file_path.value()
                 << " for reading";
    }
    return false;
  }
  int64_t len64 = file.GetLength();
  if (len64 < 0 || len64 >= (static_cast<int64_t>(1) << 31)) {
    LOG(ERROR) << "Unexpected file length for " << file_path.value() << " - "
               << len64;
    return false;
  }
  int len = static_cast<int>(len64);
  if (len == 0)
    return false;

  buffer->resize(static_cast<size_t>(len));
  int read_len = file.Read(0, reinterpret_cast<char*>(buffer->data()), len);
  if (read_len != len) {
    LOG(ERROR) << "Failed to read " << len << "bytes from "
               << file_path.value();
    return false;
  }
  return true;
}

// static
scoped_refptr<base::RefCountedMemory>
VivaldiDataSourcesAPI::ReadFileOnBlockingThread(
    const base::FilePath& file_path) {
  std::vector<unsigned char> buffer;
  if (ReadFileOnBlockingThread(file_path, &buffer)) {
    return base::RefCountedBytes::TakeVector(&buffer);
  }
  return scoped_refptr<base::RefCountedMemory>();
}

void VivaldiDataSourcesAPI::LoadMappings() {
  sequence_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&VivaldiDataSourcesAPI::LoadMappingsOnFileThread, this));
}

void VivaldiDataSourcesAPI::LoadMappingsOnFileThread() {
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());
  DCHECK(path_id_map_.empty());

  base::FilePath file_path = GetFileMappingFilePath();
  std::vector<unsigned char> data;
  if (!ReadFileOnBlockingThread(file_path, &data))
    return;

  base::JSONReader json_reader;
  base::Optional<base::Value> root = json_reader.ReadToValue(
      base::StringPiece(reinterpret_cast<char*>(data.data()), data.size()));
  if (!root) {
    LOG(ERROR) << file_path.value() << " is not a valid JSON - "
               << json_reader.GetErrorMessage();
    return;
  }

  if (root->is_dict()) {
    if (const base::Value* mappings_value = root->FindDictKey("mappings")) {
      InitMappingsOnFileThread(
          static_cast<const base::DictionaryValue*>(mappings_value));
    }
  }
}

void VivaldiDataSourcesAPI::InitMappingsOnFileThread(
    const base::DictionaryValue* dict) {
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());
  DCHECK(path_id_map_.empty());

  for (const auto& i : dict->DictItems()) {
    const std::string& id = i.first;
    if (isOldFormatThumbnailId(id)) {
      // Older mapping entry that we just skip as we know the path statically.
      continue;
    }
    const base::Value& value = i.second;
    if (value.is_dict()) {
      const std::string* path_string = value.FindStringKey("local_path");
      if (!path_string) {
        // Older format support.
        path_string = value.FindStringKey("relative_path");
      }
      if (path_string) {
#if defined(OS_POSIX)
        base::FilePath path(*path_string);
#elif defined(OS_WIN)
        base::FilePath path(base::UTF8ToWide(*path_string));
#endif
        path_id_map_.emplace(id, std::move(path));
        continue;
      }
    }
    LOG(WARNING) << "Invalid entry " << id << " in \""
                 << kDatasourceFilemappingFilename << "\" file.";
  }
}

std::string VivaldiDataSourcesAPI::GetMappingJSONOnFileThread() {
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());

  // TODO(igor@vivaldi.com): Write the mapping file even if there are no
  // entries. This allows in future to write a URL format converter for
  // bookamrks and a add a version field to the file. Then presence of the file
  // without the version string will indicate the need for converssion.

  std::vector<base::Value::DictStorage::value_type> items;
  for (const auto& it : path_id_map_) {
    const base::FilePath& path = it.second;
    auto item = std::make_unique<base::Value>(base::Value::Type::DICTIONARY);
    item->SetStringKey("local_path", path.value());
    items.emplace_back(it.first, std::move(item));
  }

  base::Value root(base::Value::Type::DICTIONARY);
  root.SetKey("mappings",
              base::Value(base::Value::DictStorage(std::move(items))));

  std::string json;
  base::JSONWriter::WriteWithOptions(
      root, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
  return json;
}

void VivaldiDataSourcesAPI::SaveMappingsOnFileThread() {
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());

  std::string json = GetMappingJSONOnFileThread();
  base::FilePath path = GetFileMappingFilePath();
  if (json.empty()) {
    if (!base::DeleteFile(path, false)) {
      LOG(ERROR) << "failed to delete " << path.value();
    }
    return;
  }

  if (json.length() >= (1U << 31)) {
    LOG(ERROR) << "the size to write is too big - " << json.length();
    return;
  }

  // Write via a temporary to prevent leaving a corrupted file on browser
  // crashes, disk full etc. Note that still can leave the file corrupted
  // on OS crashes or power loss, but loosing thumbnails is not the end of
  // the world.

  base::FilePath tmp_path =
      user_data_dir_.AppendASCII(kDatasourceFilemappingTmpFilename);

  int length = static_cast<int>(json.length());
  if (length != base::WriteFile(tmp_path, json.data(), length)) {
    LOG(ERROR) << "Failed to write to " << tmp_path.value() << " " << length
               << " bytes";
    return;
  }
  if (!base::ReplaceFile(tmp_path, path, nullptr)) {
    LOG(ERROR) << "Failed to rename " << tmp_path.value() << " to "
               << path.value();
  }
}

base::FilePath VivaldiDataSourcesAPI::GetFileMappingFilePath() {
  return user_data_dir_.AppendASCII(kDatasourceFilemappingFilename);
}

base::FilePath VivaldiDataSourcesAPI::GetThumbnailPath(
    base::StringPiece thumbnail_id) {
  base::FilePath path = user_data_dir_.Append(kThumbnailDirectory);
#if defined(OS_POSIX)
  path = path.Append(thumbnail_id);
#elif defined(OS_WIN)
  path = path.Append(base::UTF8ToUTF16(thumbnail_id));
#endif
  return path;
}

// static
bool VivaldiDataSourcesAPI::IsBookmarkCapureUrl(const std::string& url) {
  UrlKind url_kind;
  std::string id;
  return ParseDataUrl(url, &url_kind, &id) && url_kind == THUMBNAIL_URL;
}

// static
bool VivaldiDataSourcesAPI::ParseDataUrl(base::StringPiece url,
                                         UrlKind* url_kind,
                                         std::string* id) {
  if (url.empty())
    return false;

  // Special-case resource URLs that do not have scheme or path components
  if (url.starts_with("/resources/"))
    return false;

  GURL gurl(url);
  if (!gurl.is_valid()) {
    LOG(WARNING) << "The url argument is not a valid URL - " << url;
    return false;
  }

  if (gurl.SchemeIs(VIVALDI_DATA_URL_SCHEME) &&
      gurl.host_piece() == VIVALDI_DATA_URL_HOST) {
    base::StringPiece path = gurl.path_piece();
    base::StringPiece prefix = "/" VIVALDI_DATA_URL_PATH_MAPPING_DIR "/";
    if (path.starts_with(prefix)) {
      path.remove_prefix(prefix.length());
      path.CopyToString(id);
      if (isOldFormatThumbnailId(path)) {
        *url_kind = VivaldiDataSourcesAPI::THUMBNAIL_URL;
        *id += ".png";
      } else {
        *url_kind = VivaldiDataSourcesAPI::PATH_MAPPING_URL;
      }
      return true;
    }
    prefix = "/" VIVALDI_DATA_URL_THUMBNAIL_DIR "/";
    if (path.starts_with(prefix)) {
      path.remove_prefix(prefix.length());
      path.CopyToString(id);
      *url_kind = VivaldiDataSourcesAPI::THUMBNAIL_URL;
      return true;
    }
  }
  return false;
}

// static
std::string VivaldiDataSourcesAPI::MakeDataUrl(UrlKind url_kind,
                                              base::StringPiece id) {
  std::string url = (url_kind == PATH_MAPPING_URL)
                        ? ::vivaldi::kBasePathMappingUrl
                        : ::vivaldi::kBaseThumbnailUrl;
  id.AppendToString(&url);
  return url;
}

// static
bool VivaldiDataSourcesAPI::isOldFormatThumbnailId(base::StringPiece id) {
  int64_t bookmark_id;
  return id.length() <= 20 && base::StringToInt64(id, &bookmark_id);
}

// static
void VivaldiDataSourcesAPI::ScheduleRemovalOfUnusedUrlData(
    content::BrowserContext* browser_context,
    base::TimeDelta when) {
  VivaldiDataSourcesAPI* api = FromBrowserContext(browser_context);
  DCHECK(api);
  if (!api) {
    return;
  }

  base::PostDelayedTask(
      FROM_HERE,
      {content::BrowserThread::UI,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&VivaldiDataSourcesAPI::CollectUsedUrlsOnUIThread, api),
      when);
}

void VivaldiDataSourcesAPI::CollectUsedUrlsOnUIThread() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile_)
    return;

  UrlKind url_kind;
  std::string id;
  UsedIds used_ids;

  // Collect data url ids from bookmarks.
  BookmarkModel* model = BookmarkModelFactory::GetForBrowserContext(profile_);
  DCHECK(model->loaded());
  if (!model->loaded())
    return;

  ui::TreeNodeIterator<const BookmarkNode> iterator(model->root_node());
  while (iterator.has_next()) {
    const BookmarkNode* node = iterator.Next();
    std::string thumbnail = base::UTF16ToUTF8(node->GetThumbnail());
    if (ParseDataUrl(thumbnail, &url_kind, &id)) {
      used_ids[url_kind].push_back(std::move(id));
    }
  }

  // Collect data url ids from preferences.
  PrefService* pref_service = profile_->GetPrefs();
  for (int i = 0; i < VivaldiDataSourcesAPI::kDataMappingPrefsCount; ++i) {
    std::string preference_value =
        pref_service->GetString(VivaldiDataSourcesAPI::kDataMappingPrefs[i]);
    if (ParseDataUrl(preference_value, &url_kind, &id)) {
      used_ids[url_kind].push_back(std::move(id));
    }
  }

  sequence_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&VivaldiDataSourcesAPI::RemoveUnusedUrlDataOnFileThread,
                     this, std::move(used_ids)));
}

void VivaldiDataSourcesAPI::RemoveUnusedUrlDataOnFileThread(UsedIds used_ids) {
  static_assert(kUrlKindCount == 2, "The code supports 2 url kinds");
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());

  // Collect newly allocated ids that have not been stored in bookmarks or
  // preferences yet.
  for (int i = 0; i < kUrlKindCount; ++i) {
    const std::vector<std::string>* v = &file_thread_newborn_ids_[i];
    used_ids[i].insert(used_ids[i].end(), v->cbegin(), v->cend());
  }

  base::flat_set<std::string> used_path_mapping_set(
      std::move(used_ids[PATH_MAPPING_URL]));

  size_t removed_path_mappings = 0;
  for (auto i = path_id_map_.begin(); i != path_id_map_.end();) {
    // Update the iterator before the following erase call.
    auto current = i;
    i++;
    if (!used_path_mapping_set.contains(current->first)) {
      base::PostTask(
          FROM_HERE, {content::BrowserThread::IO},
          base::BindOnce(&VivaldiDataSourcesAPI::ClearCacheOnIOThread, this,
                         PATH_MAPPING_URL, current->first));
      path_id_map_.erase(current);
      removed_path_mappings++;
    }
  }
  if (removed_path_mappings) {
    LOG(INFO) << removed_path_mappings
              << " unused local path mappings were removed";
    SaveMappingsOnFileThread();
  }

  base::flat_set<std::string> used_thumbnail_set(
      std::move(used_ids[THUMBNAIL_URL]));
  base::FileEnumerator files(user_data_dir_.Append(kThumbnailDirectory), false,
                             base::FileEnumerator::FILES);
  size_t removed_thumbnails = 0;
  for (base::FilePath path = files.Next(); !path.empty(); path = files.Next()) {
    base::FilePath base_name = path.BaseName();
    std::string id = base_name.AsUTF8Unsafe();
    if (!used_thumbnail_set.contains(id)) {
      base::PostTask(
          FROM_HERE, {content::BrowserThread::IO},
          base::BindOnce(&VivaldiDataSourcesAPI::ClearCacheOnIOThread, this,
                         PATH_MAPPING_URL, id));
      if (!base::DeleteFile(path, false)) {
        LOG(WARNING) << "Failed to remove thumbnail file " << path;
      }
      removed_thumbnails++;
    }
  }
  if (removed_thumbnails) {
    LOG(INFO) << removed_thumbnails << " unused thumbnail files were removed";
  }
}

void VivaldiDataSourcesAPI::AddNewbornIdOnFileThread(UrlKind url_kind,
                                                     const std::string& id) {
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());
  file_thread_newborn_ids_[url_kind].push_back(id);
}

void VivaldiDataSourcesAPI::RemoveNewbornIdOnFileThread(UrlKind url_kind,
                                                        const std::string& id) {
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());
  std::vector<std::string>* v = &file_thread_newborn_ids_[url_kind];
  auto i = std::find(v->begin(), v->end(), id);
  if (i != v->end()) {
    v->erase(i);
  } else {
    // This should only be called for active ids.
    NOTREACHED() << url_kind << " " << id;
  }
}

// static
void VivaldiDataSourcesAPI::UpdateMapping(
    content::BrowserContext* browser_context,
    int64_t bookmark_id,
    int preference_index,
    base::FilePath file_path,
    UpdateMappingCallback callback) {
  // Exactly one of bookmarkId, preference_index must be given.
  DCHECK_NE(bookmark_id == 0, preference_index == -1);
  DCHECK(0 <= bookmark_id);
  DCHECK(-1 <= preference_index || preference_index < kDataMappingPrefsCount);
  VivaldiDataSourcesAPI* api = FromBrowserContext(browser_context);
  DCHECK(api);
  if (!api) {
    std::move(callback).Run(false);
    return;
  }

  api->sequence_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&VivaldiDataSourcesAPI::UpdateMappingOnFileThread, api,
                     bookmark_id, preference_index, std::move(file_path),
                     std::move(callback)));
}

void VivaldiDataSourcesAPI::UpdateMappingOnFileThread(
    int64_t bookmark_id,
    int preference_index,
    base::FilePath file_path,
    UpdateMappingCallback callback) {
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());

  std::string path_id = base::GenerateGUID();
  AddNewbornIdOnFileThread(PATH_MAPPING_URL, path_id);

  path_id_map_.emplace(path_id, std::move(file_path));

  base::PostTask(
      FROM_HERE, {content::BrowserThread::UI},
      base::BindOnce(&VivaldiDataSourcesAPI::FinishUpdateMappingOnUIThread,
                     this, bookmark_id, preference_index, std::move(path_id),
                     std::move(callback)));
  SaveMappingsOnFileThread();
}

void VivaldiDataSourcesAPI::FinishUpdateMappingOnUIThread(
    int64_t bookmark_id,
    int preference_index,
    std::string path_id,
    UpdateMappingCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // profile_ is null on shutdown.
  bool success = false;
  if (profile_) {
    std::string url = MakeDataUrl(PATH_MAPPING_URL, path_id);
    if (bookmark_id > 0) {
      std::string old_url;
      success = VivaldiBookmarksAPI::SetBookmarkThumbnail(profile_, bookmark_id,
                                                          url, &old_url);
      UrlKind old_kind;
      std::string old_id;
      if (success && ParseDataUrl(old_url, &old_kind, &old_id)) {
        base::PostTask(
            FROM_HERE, {content::BrowserThread::IO},
            base::BindOnce(&VivaldiDataSourcesAPI::ClearCacheOnIOThread, this,
                           old_kind, old_id));
      }
    } else {
      profile_->GetPrefs()->SetString(kDataMappingPrefs[preference_index], url);
      success = true;
    }
  }
  sequence_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&VivaldiDataSourcesAPI::RemoveNewbornIdOnFileThread, this,
                                PATH_MAPPING_URL, path_id));

  std::move(callback).Run(success);
}

void VivaldiDataSourcesAPI::SetCacheOnIOThread(
    UrlKind url_kind, std::string id,
    scoped_refptr<base::RefCountedMemory> data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  DCHECK(data);
  io_thread_data_cache_[url_kind][id] = std::move(data);
}

void VivaldiDataSourcesAPI::ClearCacheOnIOThread(UrlKind url_kind,
                                                 std::string id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  io_thread_data_cache_[url_kind].erase(id);
}

void VivaldiDataSourcesAPI::GetDataForId(
    UrlKind url_kind,
    std::string id,
    content::URLDataSource::GotDataCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);

  if (url_kind == PATH_MAPPING_URL) {
    if (isOldFormatThumbnailId(id)) {
      url_kind = THUMBNAIL_URL;
      id += ".png";
    }
  } else {
    DCHECK(url_kind == THUMBNAIL_URL);
  }

  auto it = io_thread_data_cache_[url_kind].find(id);
  if (it != io_thread_data_cache_[url_kind].end()) {
    callback.Run(it->second);
    return;
  }

  sequence_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&VivaldiDataSourcesAPI::GetDataForIdOnFileThread, this,
                     url_kind, std::move(id), std::move(callback)));
}

void VivaldiDataSourcesAPI::GetDataForIdOnFileThread(
    UrlKind url_kind, std::string id,
    content::URLDataSource::GotDataCallback callback) {
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());

  base::FilePath file_path;
  if (url_kind == THUMBNAIL_URL) {
    file_path = GetThumbnailPath(id);
  } else {
    DCHECK(url_kind == PATH_MAPPING_URL);

    // It is not an error if id is not in the map. The IO thread may not
    // be aware yet that the id was removed when it called this.
    auto it = path_id_map_.find(id);
    if (it != path_id_map_.end()) {
      file_path = it->second;
      if (!file_path.IsAbsolute()) {
        file_path = user_data_dir_.Append(file_path);
      }
    }
  }

  scoped_refptr<base::RefCountedMemory> data;
  if (!file_path.empty()) {
    data = ReadFileOnBlockingThread(file_path);
  }

  base::PostTask(
      FROM_HERE, {content::BrowserThread::IO},
      base::BindOnce(&VivaldiDataSourcesAPI::FinishGetDataForIdOnIOThread, this,
                     url_kind, std::move(id), std::move(data),
                     std::move(callback)));
}

void VivaldiDataSourcesAPI::FinishGetDataForIdOnIOThread(
    UrlKind url_kind,
    std::string id,
    scoped_refptr<base::RefCountedMemory> data,
    content::URLDataSource::GotDataCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);

  if (data) {
    SetCacheOnIOThread(url_kind, std::move(id), data);
  }
  callback.Run(std::move(data));
}

// static
void VivaldiDataSourcesAPI::AddImageDataForBookmark(
    content::BrowserContext* browser_context,
    int64_t bookmark_id,
    scoped_refptr<base::RefCountedMemory> png_data,
    AddBookmarkImageCallback callback) {
  DCHECK(png_data->size() > 0);
  VivaldiDataSourcesAPI* api = FromBrowserContext(browser_context);
  DCHECK(api);
  if (!api) {
    std::move(callback).Run(false);
    return;
  }
  api->AddImageDataForBookmark(bookmark_id, std::move(png_data),
                               std::move(callback));
}

void VivaldiDataSourcesAPI::AddImageDataForBookmark(
    int64_t bookmark_id,
    scoped_refptr<base::RefCountedMemory> png_data,
    AddBookmarkImageCallback ui_thread_callback) {
  DCHECK(png_data->size() > 0);

  sequence_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &VivaldiDataSourcesAPI::AddImageDataForBookmarkOnFileThread, this,
          bookmark_id, std::move(png_data), std::move(ui_thread_callback)));
}

void VivaldiDataSourcesAPI::AddImageDataForBookmarkOnFileThread(
    int64_t bookmark_id,
    scoped_refptr<base::RefCountedMemory> png_data,
    AddBookmarkImageCallback ui_thread_callback) {
  DCHECK(sequence_task_runner_->RunsTasksInCurrentSequence());

  bool success = false;
  std::string thumbnail_id = base::GenerateGUID() + ".png";
  AddNewbornIdOnFileThread(THUMBNAIL_URL, thumbnail_id);

  base::FilePath path = GetThumbnailPath(thumbnail_id);
  base::FilePath dir = path.DirName();
  if (!base::DirectoryExists(dir)) {
    LOG(INFO) << "Creating thumbnail directory: " << path.value();
    base::CreateDirectory(dir);
  }
  // The caller must ensure that data fit 2G.
  int bytes = base::WriteFile(path, png_data->front_as<char>(),
                              static_cast<int>(png_data->size()));
  if (bytes < 0 || static_cast<size_t>(bytes) != png_data->size()) {
    LOG(ERROR) << "Error writing to file: " << path.value();
  } else {
    success = true;

    // Populate the cache
    base::PostTask(
        FROM_HERE, {content::BrowserThread::IO},
        base::BindOnce(&VivaldiDataSourcesAPI::SetCacheOnIOThread, this,
                       THUMBNAIL_URL, thumbnail_id, std::move(png_data)));
  }

  base::PostTask(
      FROM_HERE, {content::BrowserThread::UI},
      base::BindOnce(
          &VivaldiDataSourcesAPI::FinishAddImageDataForBookmarkOnUIThread, this,
          std::move(ui_thread_callback), success, bookmark_id,
          std::move(thumbnail_id)));
}

void VivaldiDataSourcesAPI::FinishAddImageDataForBookmarkOnUIThread(
    AddBookmarkImageCallback ui_thread_callback,
    bool success,
    int64_t bookmark_id, std::string thumbnail_id) {
  if (success) {
    if (!profile_) {
      success = false;
    } else  {
      std::string url = MakeDataUrl(THUMBNAIL_URL, thumbnail_id);
      std::string old_url;
      success = VivaldiBookmarksAPI::SetBookmarkThumbnail(profile_, bookmark_id,
                                                          url, &old_url);
      UrlKind old_kind;
      std::string old_id;
      if (success && ParseDataUrl(old_url, &old_kind, &old_id)) {
        base::PostTask(
            FROM_HERE, {content::BrowserThread::IO},
            base::BindOnce(&VivaldiDataSourcesAPI::ClearCacheOnIOThread, this,
                           old_kind, old_id));
      }
    }
  }
  sequence_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&VivaldiDataSourcesAPI::RemoveNewbornIdOnFileThread, this,
                     THUMBNAIL_URL, thumbnail_id));
  std::move(ui_thread_callback).Run(success);
}

// static
void VivaldiDataSourcesAPI::InitFactory() {
  VivaldiDataSourcesAPIHolder::GetFactoryInstance();
}

VivaldiDataSourcesAPI* VivaldiDataSourcesAPI::FromBrowserContext(
    content::BrowserContext* browser_context) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  VivaldiDataSourcesAPIHolder* holder =
      VivaldiDataSourcesAPIHolder::GetFactoryInstance()->Get(browser_context);
  if (!holder)
    return nullptr;
  return holder->api_.get();
}

VivaldiDataSourcesAPIHolder::VivaldiDataSourcesAPIHolder(
    content::BrowserContext* context) {
  Profile* profile = Profile::FromBrowserContext(context);
  api_ = base::MakeRefCounted<VivaldiDataSourcesAPI>(profile);
  api_->LoadMappings();
}

VivaldiDataSourcesAPIHolder::~VivaldiDataSourcesAPIHolder() {}

// static
BrowserContextKeyedAPIFactory<VivaldiDataSourcesAPIHolder>*
VivaldiDataSourcesAPIHolder::GetFactoryInstance() {
  static base::LazyInstance<BrowserContextKeyedAPIFactory<
      VivaldiDataSourcesAPIHolder>>::DestructorAtExit factory =
      LAZY_INSTANCE_INITIALIZER;

  return factory.Pointer();
}

void VivaldiDataSourcesAPIHolder::Shutdown() {
  // Prevent farther access to api_ from UI thread. Note that if it can still
  // be used on IO or worker threads.
  api_->profile_ = nullptr;
  api_.reset();
}

}  // namespace extensions
