// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/extensions/extension.h"

#include <algorithm>

#include "app/l10n_util.h"
#include "base/base64.h"
#include "base/basictypes.h"
#include "base/command_line.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/i18n/rtl.h"
#include "base/logging.h"
#include "base/singleton.h"
#include "base/stl_util-inl.h"
#include "base/third_party/nss/blapi.h"
#include "base/third_party/nss/sha256.h"
#include "base/string_number_conversions.h"
#include "base/utf_string_conversions.h"
#include "base/values.h"
#include "base/version.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/chrome_version_info.h"
#include "chrome/common/extensions/extension_action.h"
#include "chrome/common/extensions/extension_constants.h"
#include "chrome/common/extensions/extension_error_utils.h"
#include "chrome/common/extensions/extension_l10n_util.h"
#include "chrome/common/extensions/extension_resource.h"
#include "chrome/common/extensions/user_script.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/url_constants.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "net/base/registry_controlled_domain.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "webkit/glue/image_decoder.h"

namespace keys = extension_manifest_keys;
namespace values = extension_manifest_values;
namespace errors = extension_manifest_errors;

namespace {

const int kPEMOutputColumns = 65;

// KEY MARKERS
const char kKeyBeginHeaderMarker[] = "-----BEGIN";
const char kKeyBeginFooterMarker[] = "-----END";
const char kKeyInfoEndMarker[] = "KEY-----";
const char kPublic[] = "PUBLIC";
const char kPrivate[] = "PRIVATE";

const int kRSAKeySize = 1024;

// Converts a normal hexadecimal string into the alphabet used by extensions.
// We use the characters 'a'-'p' instead of '0'-'f' to avoid ever having a
// completely numeric host, since some software interprets that as an IP
// address.
static void ConvertHexadecimalToIDAlphabet(std::string* id) {
  for (size_t i = 0; i < id->size(); ++i) {
    int val;
    if (base::HexStringToInt(id->substr(i, 1), &val))
      (*id)[i] = val + 'a';
    else
      (*id)[i] = 'a';
  }
}

const int kValidWebExtentSchemes =
    URLPattern::SCHEME_HTTP | URLPattern::SCHEME_HTTPS;

// These keys are allowed by all crx files (apps, extensions, themes, etc).
static const char* kBaseCrxKeys[] = {
  keys::kCurrentLocale,
  keys::kDefaultLocale,
  keys::kDescription,
  keys::kIcons,
  keys::kName,
  keys::kPublicKey,
  keys::kSignature,
  keys::kVersion,
  keys::kUpdateURL
};

bool IsBaseCrxKey(const std::string& key) {
  for (size_t i = 0; i < arraysize(kBaseCrxKeys); ++i) {
    if (key == kBaseCrxKeys[i])
      return true;
  }

  return false;
}

// Names of API modules that do not require a permission.
const char kBrowserActionModuleName[] = "browserAction";
const char kBrowserActionsModuleName[] = "browserActions";
const char kDevToolsModuleName[] = "devtools";
const char kExtensionModuleName[] = "extension";
const char kI18NModuleName[] = "i18n";
const char kPageActionModuleName[] = "pageAction";
const char kPageActionsModuleName[] = "pageActions";
const char kTestModuleName[] = "test";

// Names of modules that can be used without listing it in the permissions
// section of the manifest.
const char* kNonPermissionModuleNames[] = {
  kBrowserActionModuleName,
  kBrowserActionsModuleName,
  kDevToolsModuleName,
  kExtensionModuleName,
  kI18NModuleName,
  kPageActionModuleName,
  kPageActionsModuleName,
  kTestModuleName
};
const size_t kNumNonPermissionModuleNames =
    arraysize(kNonPermissionModuleNames);

// Names of functions (within modules requiring permissions) that can be used
// without asking for the module permission. In other words, functions you can
// use with no permissions specified.
const char* kNonPermissionFunctionNames[] = {
  "tabs.create",
  "tabs.update"
};
const size_t kNumNonPermissionFunctionNames =
    arraysize(kNonPermissionFunctionNames);


// A map between permission name and its install warning message.
class PermissionMap {
 public:
  static PermissionMap* GetSingleton() {
    return Singleton<PermissionMap>::get();
  }

  int GetPermissionMessageId(const std::string& permission) {
    return Extension::kPermissions[permission_map_[permission]].message_id;
  }

 private:
  friend struct DefaultSingletonTraits<PermissionMap>;

  PermissionMap() {
    for (size_t i = 0; i < Extension::kNumPermissions; ++i)
      permission_map_[Extension::kPermissions[i].name] = i;
  };

  ~PermissionMap() { }

  std::map<const std::string, size_t> permission_map_;
};


// Aliased to kTabPermission for purposes of API checks, but not allowed
// in the permissions field of the manifest.
static const char kWindowPermission[] = "windows";

}  // namespace


const FilePath::CharType Extension::kManifestFilename[] =
    FILE_PATH_LITERAL("manifest.json");
const FilePath::CharType Extension::kLocaleFolder[] =
    FILE_PATH_LITERAL("_locales");
const FilePath::CharType Extension::kMessagesFilename[] =
    FILE_PATH_LITERAL("messages.json");

#if defined(OS_WIN)
const char Extension::kExtensionRegistryPath[] =
    "Software\\Google\\Chrome\\Extensions";
#endif

// first 16 bytes of SHA256 hashed public key.
const size_t Extension::kIdSize = 16;

const char Extension::kMimeType[] = "application/x-chrome-extension";

const int Extension::kIconSizes[] = {
  EXTENSION_ICON_LARGE,
  EXTENSION_ICON_MEDIUM,
  EXTENSION_ICON_SMALL,
  EXTENSION_ICON_SMALLISH,
  EXTENSION_ICON_BITTY
};

const int Extension::kPageActionIconMaxSize = 19;
const int Extension::kBrowserActionIconMaxSize = 19;

// Explicit permissions -- permission declaration required.
const char Extension::kBackgroundPermission[] = "background";
const char Extension::kContextMenusPermission[] = "contextMenus";
const char Extension::kBookmarkPermission[] = "bookmarks";
const char Extension::kCookiePermission[] = "cookies";
const char Extension::kExperimentalPermission[] = "experimental";
const char Extension::kGeolocationPermission[] = "geolocation";
const char Extension::kHistoryPermission[] = "history";
const char Extension::kIdlePermission[] = "idle";
const char Extension::kNotificationPermission[] = "notifications";
const char Extension::kProxyPermission[] = "proxy";
const char Extension::kTabPermission[] = "tabs";
const char Extension::kUnlimitedStoragePermission[] = "unlimitedStorage";
const char Extension::kWebstorePrivatePermission[] = "webstorePrivate";

// In general, all permissions should have an install message.
// See ExtensionsTest.PermissionMessages for an explanation of each
// exception.
const Extension::Permission Extension::kPermissions[] = {
  { kBackgroundPermission, 0 },
  { kBookmarkPermission, IDS_EXTENSION_PROMPT2_WARNING_BOOKMARKS },
  { kContextMenusPermission, 0 },
  { kCookiePermission, 0 },
  { kExperimentalPermission, 0 },
  { kGeolocationPermission, IDS_EXTENSION_PROMPT2_WARNING_GEOLOCATION },
  { kIdlePermission, 0 },
  { kHistoryPermission, IDS_EXTENSION_PROMPT2_WARNING_BROWSING_HISTORY },
  { kNotificationPermission, 0 },
  { kProxyPermission, 0 },
  { kTabPermission, IDS_EXTENSION_PROMPT2_WARNING_BROWSING_HISTORY },
  { kUnlimitedStoragePermission, 0 },
  { kWebstorePrivatePermission, 0 },
};
const size_t Extension::kNumPermissions =
    arraysize(Extension::kPermissions);

const char* const Extension::kHostedAppPermissionNames[] = {
  Extension::kBackgroundPermission,
  Extension::kGeolocationPermission,
  Extension::kNotificationPermission,
  Extension::kUnlimitedStoragePermission,
  Extension::kWebstorePrivatePermission,
};
const size_t Extension::kNumHostedAppPermissions =
    arraysize(Extension::kHostedAppPermissionNames);

// We purposefully don't put this into kPermissionNames.
const char Extension::kOldUnlimitedStoragePermission[] = "unlimited_storage";

//
// Extension::StaticData
//

Extension::StaticData::StaticData()
    : incognito_split_mode(false) {
}

Extension::StaticData::~StaticData() {
}

//
// Extension
//

// static
int Extension::GetPermissionMessageId(const std::string& permission) {
  return PermissionMap::GetSingleton()->GetPermissionMessageId(permission);
}

std::vector<string16> Extension::GetPermissionMessages() {
  std::vector<string16> messages;
  if (!plugins_.empty()) {
    messages.push_back(
        l10n_util::GetStringUTF16(IDS_EXTENSION_PROMPT2_WARNING_FULL_ACCESS));
    return messages;
  }

  string16 host_msg = GetHostPermissionMessage();
  if (!host_msg.empty())
    messages.push_back(host_msg);

  std::set<string16> simple_msgs = GetSimplePermissionMessages();
  messages.insert(messages.end(), simple_msgs.begin(), simple_msgs.end());

  return messages;
}

std::set<string16> Extension::GetSimplePermissionMessages() {
  std::set<string16> messages;
  std::set<std::string>::const_iterator i;
  for (i = api_permissions().begin(); i != api_permissions().end(); ++i) {
    int message_id = GetPermissionMessageId(*i);
    if (message_id)
      messages.insert(l10n_util::GetStringUTF16(message_id));
  }
  return messages;
}

std::vector<std::string> Extension::GetDistinctHosts() {
  return GetDistinctHosts(GetEffectiveHostPermissions().patterns());
}

// static
std::vector<std::string> Extension::GetDistinctHosts(
    const URLPatternList& host_patterns) {

  // Vector because we later want to access these by index.
  std::vector<std::string> distinct_hosts;

  std::set<std::string> rcd_set;
  for (size_t i = 0; i < host_patterns.size(); ++i) {
    std::string candidate = host_patterns[i].host();
    size_t registry = net::RegistryControlledDomainService::GetRegistryLength(
        candidate, false);
    if (registry && registry != std::string::npos) {
      std::string no_rcd(candidate, 0, candidate.size() - registry);
      if (rcd_set.count(no_rcd))
        continue;
      rcd_set.insert(no_rcd);
    }
    if (std::find(distinct_hosts.begin(), distinct_hosts.end(), candidate) ==
                  distinct_hosts.end()) {
      distinct_hosts.push_back(candidate);
    }
  }

  return distinct_hosts;
}

string16 Extension::GetHostPermissionMessage() {
  if (HasEffectiveAccessToAllHosts())
    return l10n_util::GetStringUTF16(IDS_EXTENSION_PROMPT2_WARNING_ALL_HOSTS);

  std::vector<std::string> hosts = GetDistinctHosts();
  if (hosts.size() == 1) {
    return l10n_util::GetStringFUTF16(IDS_EXTENSION_PROMPT2_WARNING_1_HOST,
                                      UTF8ToUTF16(hosts[0]));
  } else if (hosts.size() == 2) {
    return l10n_util::GetStringFUTF16(IDS_EXTENSION_PROMPT2_WARNING_2_HOSTS,
                                      UTF8ToUTF16(hosts[0]),
                                      UTF8ToUTF16(hosts[1]));
  } else if (hosts.size() == 3) {
    return l10n_util::GetStringFUTF16(IDS_EXTENSION_PROMPT2_WARNING_3_HOSTS,
                                      UTF8ToUTF16(hosts[0]),
                                      UTF8ToUTF16(hosts[1]),
                                      UTF8ToUTF16(hosts[2]));
  } else if (hosts.size() >= 4) {
    return l10n_util::GetStringFUTF16(
        IDS_EXTENSION_PROMPT2_WARNING_4_OR_MORE_HOSTS,
        UTF8ToUTF16(hosts[0]),
        UTF8ToUTF16(hosts[1]),
        base::IntToString16(hosts.size() - 2));
  }

  return string16();
}

// static
bool Extension::IsHostedAppPermission(const std::string& str) {
  for (size_t i = 0; i < Extension::kNumHostedAppPermissions; ++i) {
    if (str == Extension::kHostedAppPermissionNames[i]) {
      return true;
    }
  }
  return false;
}

Extension::~Extension() {
}

const std::string Extension::VersionString() const {
  return version_->GetString();
}

// static
bool Extension::IsExtension(const FilePath& file_name) {
  return file_name.MatchesExtension(chrome::kExtensionFileExtension);
}

// static
bool Extension::IdIsValid(const std::string& id) {
  // Verify that the id is legal.
  if (id.size() != (kIdSize * 2))
    return false;

  // We only support lowercase IDs, because IDs can be used as URL components
  // (where GURL will lowercase it).
  std::string temp = StringToLowerASCII(id);
  for (size_t i = 0; i < temp.size(); i++)
    if (temp[i] < 'a' || temp[i] > 'p')
      return false;

  return true;
}

// static
GURL Extension::GetResourceURL(const GURL& extension_url,
                               const std::string& relative_path) {
  DCHECK(extension_url.SchemeIs(chrome::kExtensionScheme));
  DCHECK_EQ("/", extension_url.path());

  GURL ret_val = GURL(extension_url.spec() + relative_path);
  DCHECK(StartsWithASCII(ret_val.spec(), extension_url.spec(), false));

  return ret_val;
}

bool Extension::GenerateId(const std::string& input, std::string* output) {
  CHECK(output);
  if (input.length() == 0)
    return false;

  const uint8* ubuf = reinterpret_cast<const unsigned char*>(input.data());
  SHA256Context ctx;
  SHA256_Begin(&ctx);
  SHA256_Update(&ctx, ubuf, input.length());
  uint8 hash[Extension::kIdSize];
  SHA256_End(&ctx, hash, NULL, sizeof(hash));
  *output = StringToLowerASCII(base::HexEncode(hash, sizeof(hash)));
  ConvertHexadecimalToIDAlphabet(output);

  return true;
}

// Helper method that loads a UserScript object from a dictionary in the
// content_script list of the manifest.
bool Extension::LoadUserScriptHelper(const DictionaryValue* content_script,
                                     int definition_index, std::string* error,
                                     UserScript* result) {
  // run_at
  if (content_script->HasKey(keys::kRunAt)) {
    std::string run_location;
    if (!content_script->GetString(keys::kRunAt, &run_location)) {
      *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidRunAt,
          base::IntToString(definition_index));
      return false;
    }

    if (run_location == values::kRunAtDocumentStart) {
      result->set_run_location(UserScript::DOCUMENT_START);
    } else if (run_location == values::kRunAtDocumentEnd) {
      result->set_run_location(UserScript::DOCUMENT_END);
    } else if (run_location == values::kRunAtDocumentIdle) {
      result->set_run_location(UserScript::DOCUMENT_IDLE);
    } else {
      *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidRunAt,
          base::IntToString(definition_index));
      return false;
    }
  }

  // all frames
  if (content_script->HasKey(keys::kAllFrames)) {
    bool all_frames = false;
    if (!content_script->GetBoolean(keys::kAllFrames, &all_frames)) {
      *error = ExtensionErrorUtils::FormatErrorMessage(
            errors::kInvalidAllFrames, base::IntToString(definition_index));
      return false;
    }
    result->set_match_all_frames(all_frames);
  }

  // matches
  ListValue* matches = NULL;
  if (!content_script->GetList(keys::kMatches, &matches)) {
    *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidMatches,
        base::IntToString(definition_index));
    return false;
  }

  if (matches->GetSize() == 0) {
    *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidMatchCount,
        base::IntToString(definition_index));
    return false;
  }
  for (size_t j = 0; j < matches->GetSize(); ++j) {
    std::string match_str;
    if (!matches->GetString(j, &match_str)) {
      *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidMatch,
          base::IntToString(definition_index), base::IntToString(j));
      return false;
    }

    URLPattern pattern(UserScript::kValidUserScriptSchemes);
    if (!pattern.Parse(match_str)) {
      *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidMatch,
          base::IntToString(definition_index), base::IntToString(j));
      return false;
    }

    result->add_url_pattern(pattern);
  }

  // include/exclude globs (mostly for Greasemonkey compatibility)
  if (!LoadGlobsHelper(content_script, definition_index, keys::kIncludeGlobs,
                       error, &UserScript::add_glob, result)) {
      return false;
  }

  if (!LoadGlobsHelper(content_script, definition_index, keys::kExcludeGlobs,
                       error, &UserScript::add_exclude_glob, result)) {
      return false;
  }

  // js and css keys
  ListValue* js = NULL;
  if (content_script->HasKey(keys::kJs) &&
      !content_script->GetList(keys::kJs, &js)) {
    *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidJsList,
        base::IntToString(definition_index));
    return false;
  }

  ListValue* css = NULL;
  if (content_script->HasKey(keys::kCss) &&
      !content_script->GetList(keys::kCss, &css)) {
    *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidCssList,
        base::IntToString(definition_index));
    return false;
  }

  // The manifest needs to have at least one js or css user script definition.
  if (((js ? js->GetSize() : 0) + (css ? css->GetSize() : 0)) == 0) {
    *error = ExtensionErrorUtils::FormatErrorMessage(errors::kMissingFile,
        base::IntToString(definition_index));
    return false;
  }

  if (js) {
    for (size_t script_index = 0; script_index < js->GetSize();
         ++script_index) {
      Value* value;
      std::string relative;
      if (!js->Get(script_index, &value) || !value->GetAsString(&relative)) {
        *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidJs,
            base::IntToString(definition_index),
            base::IntToString(script_index));
        return false;
      }
      GURL url = GetResourceURL(relative);
      ExtensionResource resource = GetResource(relative);
      result->js_scripts().push_back(UserScript::File(
          resource.extension_root(), resource.relative_path(), url));
    }
  }

  if (css) {
    for (size_t script_index = 0; script_index < css->GetSize();
         ++script_index) {
      Value* value;
      std::string relative;
      if (!css->Get(script_index, &value) || !value->GetAsString(&relative)) {
        *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidCss,
            base::IntToString(definition_index),
            base::IntToString(script_index));
        return false;
      }
      GURL url = GetResourceURL(relative);
      ExtensionResource resource = GetResource(relative);
      result->css_scripts().push_back(UserScript::File(
          resource.extension_root(), resource.relative_path(), url));
    }
  }

  return true;
}

bool Extension::LoadGlobsHelper(
    const DictionaryValue* content_script,
    int content_script_index,
    const char* globs_property_name,
    std::string* error,
    void(UserScript::*add_method)(const std::string& glob),
    UserScript *instance) {
  if (!content_script->HasKey(globs_property_name))
    return true;  // they are optional

  ListValue* list = NULL;
  if (!content_script->GetList(globs_property_name, &list)) {
    *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidGlobList,
        base::IntToString(content_script_index),
        globs_property_name);
    return false;
  }

  for (size_t i = 0; i < list->GetSize(); ++i) {
    std::string glob;
    if (!list->GetString(i, &glob)) {
      *error = ExtensionErrorUtils::FormatErrorMessage(errors::kInvalidGlob,
          base::IntToString(content_script_index),
          globs_property_name,
          base::IntToString(i));
      return false;
    }

    (instance->*add_method)(glob);
  }

  return true;
}

ExtensionAction* Extension::LoadExtensionActionHelper(
    const DictionaryValue* extension_action, std::string* error) {
  scoped_ptr<ExtensionAction> result(new ExtensionAction());
  result->set_extension_id(id());

  // Page actions are hidden by default, and browser actions ignore
  // visibility.
  result->SetIsVisible(ExtensionAction::kDefaultTabId, false);

  // TODO(EXTENSIONS_DEPRECATED): icons list is obsolete.
  ListValue* icons = NULL;
  if (extension_action->HasKey(keys::kPageActionIcons) &&
      extension_action->GetList(keys::kPageActionIcons, &icons)) {
    for (ListValue::const_iterator iter = icons->begin();
         iter != icons->end(); ++iter) {
      std::string path;
      if (!(*iter)->GetAsString(&path) || path.empty()) {
        *error = errors::kInvalidPageActionIconPath;
        return NULL;
      }

      result->icon_paths()->push_back(path);
    }
  }

  // TODO(EXTENSIONS_DEPRECATED): Read the page action |id| (optional).
  std::string id;
  if (extension_action->HasKey(keys::kPageActionId)) {
    if (!extension_action->GetString(keys::kPageActionId, &id)) {
      *error = errors::kInvalidPageActionId;
      return NULL;
    }
    result->set_id(id);
  }

  std::string default_icon;
  // Read the page action |default_icon| (optional).
  if (extension_action->HasKey(keys::kPageActionDefaultIcon)) {
    if (!extension_action->GetString(keys::kPageActionDefaultIcon,
                                     &default_icon) ||
        default_icon.empty()) {
      *error = errors::kInvalidPageActionIconPath;
      return NULL;
    }
    result->set_default_icon_path(default_icon);
  }

  // Read the page action title from |default_title| if present, |name| if not
  // (both optional).
  std::string title;
  if (extension_action->HasKey(keys::kPageActionDefaultTitle)) {
    if (!extension_action->GetString(keys::kPageActionDefaultTitle, &title)) {
      *error = errors::kInvalidPageActionDefaultTitle;
      return NULL;
    }
  } else if (extension_action->HasKey(keys::kName)) {
    if (!extension_action->GetString(keys::kName, &title)) {
      *error = errors::kInvalidPageActionName;
      return NULL;
    }
  }
  result->SetTitle(ExtensionAction::kDefaultTabId, title);

  // Read the action's |popup| (optional).
  const char* popup_key = NULL;
  if (extension_action->HasKey(keys::kPageActionDefaultPopup))
    popup_key = keys::kPageActionDefaultPopup;

  // For backward compatibility, alias old key "popup" to new
  // key "default_popup".
  if (extension_action->HasKey(keys::kPageActionPopup)) {
    if (popup_key) {
      *error = ExtensionErrorUtils::FormatErrorMessage(
          errors::kInvalidPageActionOldAndNewKeys,
          keys::kPageActionDefaultPopup,
          keys::kPageActionPopup);
      return NULL;
    }
    popup_key = keys::kPageActionPopup;
  }

  if (popup_key) {
    DictionaryValue* popup = NULL;
    std::string url_str;

    if (extension_action->GetString(popup_key, &url_str)) {
      // On success, |url_str| is set.  Nothing else to do.
    } else if (extension_action->GetDictionary(popup_key, &popup)) {
      // TODO(EXTENSIONS_DEPRECATED): popup is now a string only.
      // Support the old dictionary format for backward compatibility.
      if (!popup->GetString(keys::kPageActionPopupPath, &url_str)) {
        *error = ExtensionErrorUtils::FormatErrorMessage(
            errors::kInvalidPageActionPopupPath, "<missing>");
        return NULL;
      }
    } else {
      *error = errors::kInvalidPageActionPopup;
      return NULL;
    }

    if (!url_str.empty()) {
      // An empty string is treated as having no popup.
      GURL url = GetResourceURL(url_str);
      if (!url.is_valid()) {
        *error = ExtensionErrorUtils::FormatErrorMessage(
            errors::kInvalidPageActionPopupPath, url_str);
        return NULL;
      }
      result->SetPopupUrl(ExtensionAction::kDefaultTabId, url);
    } else {
      DCHECK(!result->HasPopup(ExtensionAction::kDefaultTabId))
          << "Shouldn't be posible for the popup to be set.";
    }
  }

  return result.release();
}

bool Extension::ContainsNonThemeKeys(const DictionaryValue& source) {
  for (DictionaryValue::key_iterator key = source.begin_keys();
       key != source.end_keys(); ++key) {
    if (!IsBaseCrxKey(*key) && *key != keys::kTheme)
      return true;
  }
  return false;
}

bool Extension::LoadIsApp(const DictionaryValue* manifest,
                          std::string* error) {
  if (manifest->HasKey(keys::kApp)) {
    if (!apps_enabled_) {
      *error = errors::kAppsNotEnabled;
      return false;
    } else {
      is_app_ = true;
    }
  }

  return true;
}

bool Extension::LoadExtent(const DictionaryValue* manifest,
                           const char* key,
                           ExtensionExtent* extent,
                           const char* list_error,
                           const char* value_error,
                           std::string* error) {
  Value* temp = NULL;
  if (!manifest->Get(key, &temp))
    return true;

  if (temp->GetType() != Value::TYPE_LIST) {
    *error = list_error;
    return false;
  }

  ListValue* pattern_list = static_cast<ListValue*>(temp);
  for (size_t i = 0; i < pattern_list->GetSize(); ++i) {
    std::string pattern_string;
    if (!pattern_list->GetString(i, &pattern_string)) {
      *error = ExtensionErrorUtils::FormatErrorMessage(value_error,
                                                       base::UintToString(i));
      return false;
    }

    URLPattern pattern(kValidWebExtentSchemes);
    if (!pattern.Parse(pattern_string)) {
      *error = ExtensionErrorUtils::FormatErrorMessage(value_error,
                                                       base::UintToString(i));
      return false;
    }

    // Do not allow authors to claim "<all_urls>". That would make no sense.
    if (pattern.match_all_urls()) {
      *error = ExtensionErrorUtils::FormatErrorMessage(value_error,
                                                       base::UintToString(i));
      return false;
    }

    // We do not allow authors to put wildcards in their paths. Instead, we
    // imply one at the end.
    if (pattern.path().find('*') != std::string::npos) {
      *error = ExtensionErrorUtils::FormatErrorMessage(value_error,
                                                       base::UintToString(i));
      return false;
    }
    pattern.set_path(pattern.path() + '*');

    extent->AddPattern(pattern);
  }

  return true;
}

bool Extension::LoadLaunchURL(const DictionaryValue* manifest,
                              std::string* error) {
  Value* temp = NULL;

  // launch URL can be either local (to chrome-extension:// root) or an absolute
  // web URL.
  if (manifest->Get(keys::kLaunchLocalPath, &temp)) {
    if (manifest->Get(keys::kLaunchWebURL, NULL)) {
      *error = errors::kLaunchPathAndURLAreExclusive;
      return false;
    }

    std::string launch_path;
    if (!temp->GetAsString(&launch_path)) {
      *error = errors::kInvalidLaunchLocalPath;
      return false;
    }

    // Ensure the launch path is a valid relative URL.
    GURL resolved = extension_url_.Resolve(launch_path);
    if (!resolved.is_valid() || resolved.GetOrigin() != extension_url_) {
      *error = errors::kInvalidLaunchLocalPath;
      return false;
    }

    launch_local_path_ = launch_path;
  } else if (manifest->Get(keys::kLaunchWebURL, &temp)) {
    std::string launch_url;
    if (!temp->GetAsString(&launch_url)) {
      *error = errors::kInvalidLaunchWebURL;
      return false;
    }

    // Ensure the launch URL is a valid absolute URL.
    if (!GURL(launch_url).is_valid()) {
      *error = errors::kInvalidLaunchWebURL;
      return false;
    }

    launch_web_url_ = launch_url;
  } else if (is_app_) {
    *error = errors::kLaunchURLRequired;
    return false;
  }

  // If there is no extent, we default the extent based on the launch URL.
  if (web_extent().is_empty() && !launch_web_url_.empty()) {
    GURL launch_url(launch_web_url_);
    URLPattern pattern(kValidWebExtentSchemes);
    if (!pattern.SetScheme("*")) {
      *error = errors::kInvalidLaunchWebURL;
      return false;
    }
    pattern.set_host(launch_url.host());
    pattern.set_path("/*");
    mutable_static_data_->extent.AddPattern(pattern);
  }

  // In order for the --apps-gallery-url switch to work with the gallery
  // process isolation, we must insert any provided value into the component
  // app's launch url and web extent.
  if (id() == extension_misc::kWebStoreAppId) {
    GURL gallery_url(CommandLine::ForCurrentProcess()->
        GetSwitchValueASCII(switches::kAppsGalleryURL));
    if (gallery_url.is_valid()) {
      launch_web_url_ = gallery_url.spec();

      URLPattern pattern(URLPattern::SCHEME_HTTP | URLPattern::SCHEME_HTTPS);
      pattern.Parse(gallery_url.spec());
      pattern.set_path(pattern.path() + '*');
      mutable_static_data_->extent.AddPattern(pattern);
    }
  }

  return true;
}

bool Extension::LoadLaunchContainer(const DictionaryValue* manifest,
                                    std::string* error) {
  Value* temp = NULL;
  if (!manifest->Get(keys::kLaunchContainer, &temp))
    return true;

  std::string launch_container_string;
  if (!temp->GetAsString(&launch_container_string)) {
    *error = errors::kInvalidLaunchContainer;
    return false;
  }

  if (launch_container_string == values::kLaunchContainerPanel) {
    launch_container_ = extension_misc::LAUNCH_PANEL;
  } else if (launch_container_string == values::kLaunchContainerTab) {
    launch_container_ = extension_misc::LAUNCH_TAB;
  } else {
    *error = errors::kInvalidLaunchContainer;
    return false;
  }

  // Validate the container width if present.
  if (manifest->Get(keys::kLaunchWidth, &temp)) {
    if (launch_container_ != extension_misc::LAUNCH_PANEL &&
        launch_container_ != extension_misc::LAUNCH_WINDOW) {
      *error = errors::kInvalidLaunchWidthContainer;
      return false;
    }
    if (!temp->GetAsInteger(&launch_width_) || launch_width_ < 0) {
      launch_width_ = 0;
      *error = errors::kInvalidLaunchWidth;
      return false;
    }
  }

  // Validate container height if present.
  if (manifest->Get(keys::kLaunchHeight, &temp)) {
    if (launch_container_ != extension_misc::LAUNCH_PANEL &&
        launch_container_ != extension_misc::LAUNCH_WINDOW) {
      *error = errors::kInvalidLaunchHeightContainer;
      return false;
    }
    if (!temp->GetAsInteger(&launch_height_) || launch_height_ < 0) {
      launch_height_ = 0;
      *error = errors::kInvalidLaunchHeight;
      return false;
    }
  }

  return true;
}

bool Extension::EnsureNotHybridApp(const DictionaryValue* manifest,
                                   std::string* error) {
  if (web_extent().is_empty())
    return true;

  for (DictionaryValue::key_iterator key = manifest->begin_keys();
       key != manifest->end_keys(); ++key) {
    if (!IsBaseCrxKey(*key) &&
        *key != keys::kApp &&
        *key != keys::kPermissions &&
        *key != keys::kOptionsPage) {
      *error = errors::kHostedAppsCannotIncludeExtensionFeatures;
      return false;
    }
  }

  return true;
}

Extension::Extension(const FilePath& path)
    : mutable_static_data_(new StaticData),
      converted_from_user_script_(false),
      is_theme_(false),
      is_app_(false),
      launch_container_(extension_misc::LAUNCH_TAB),
      launch_width_(0),
      launch_height_(0),
      background_page_ready_(false),
      being_upgraded_(false) {
  DCHECK(path.IsAbsolute());

  static_data_ = mutable_static_data_;
  apps_enabled_ = AppsAreEnabled();
  location_ = INVALID;

#if defined(OS_WIN)
  // Normalize any drive letter to upper-case. We do this for consistency with
  // net_utils::FilePathToFileURL(), which does the same thing, to make string
  // comparisons simpler.
  std::wstring path_str = path.value();
  if (path_str.size() >= 2 && path_str[0] >= L'a' && path_str[0] <= L'z' &&
      path_str[1] == ':')
    path_str[0] += ('A' - 'a');

  mutable_static_data_->path = FilePath(path_str);
#else
  mutable_static_data_->path = path;
#endif
}

ExtensionResource Extension::GetResource(const std::string& relative_path) {
#if defined(OS_POSIX)
  FilePath relative_file_path(relative_path);
#elif defined(OS_WIN)
  FilePath relative_file_path(UTF8ToWide(relative_path));
#endif
  return ExtensionResource(id(), path(), relative_file_path);
}

ExtensionResource Extension::GetResource(const FilePath& relative_file_path) {
  return ExtensionResource(id(), path(), relative_file_path);
}

// TODO(rafaelw): Move ParsePEMKeyBytes, ProducePEM & FormatPEMForOutput to a
// util class in base:
// http://code.google.com/p/chromium/issues/detail?id=13572
bool Extension::ParsePEMKeyBytes(const std::string& input,
                                 std::string* output) {
  DCHECK(output);
  if (!output)
    return false;
  if (input.length() == 0)
    return false;

  std::string working = input;
  if (StartsWithASCII(working, kKeyBeginHeaderMarker, true)) {
    working = CollapseWhitespaceASCII(working, true);
    size_t header_pos = working.find(kKeyInfoEndMarker,
      sizeof(kKeyBeginHeaderMarker) - 1);
    if (header_pos == std::string::npos)
      return false;
    size_t start_pos = header_pos + sizeof(kKeyInfoEndMarker) - 1;
    size_t end_pos = working.rfind(kKeyBeginFooterMarker);
    if (end_pos == std::string::npos)
      return false;
    if (start_pos >= end_pos)
      return false;

    working = working.substr(start_pos, end_pos - start_pos);
    if (working.length() == 0)
      return false;
  }

  return base::Base64Decode(working, output);
}

bool Extension::ProducePEM(const std::string& input,
                                  std::string* output) {
  CHECK(output);
  if (input.length() == 0)
    return false;

  return base::Base64Encode(input, output);
}

bool Extension::FormatPEMForFileOutput(const std::string input,
                                       std::string* output,
                                       bool is_public) {
  CHECK(output);
  if (input.length() == 0)
    return false;
  *output = "";
  output->append(kKeyBeginHeaderMarker);
  output->append(" ");
  output->append(is_public ? kPublic : kPrivate);
  output->append(" ");
  output->append(kKeyInfoEndMarker);
  output->append("\n");
  for (size_t i = 0; i < input.length(); ) {
    int slice = std::min<int>(input.length() - i, kPEMOutputColumns);
    output->append(input.substr(i, slice));
    output->append("\n");
    i += slice;
  }
  output->append(kKeyBeginFooterMarker);
  output->append(" ");
  output->append(is_public ? kPublic : kPrivate);
  output->append(" ");
  output->append(kKeyInfoEndMarker);
  output->append("\n");

  return true;
}

// static
// TODO(aa): A problem with this code is that we silently allow upgrades to
// extensions that require less permissions than the current version, but then
// we don't silently allow them to go back. In order to fix this, we would need
// to remember the max set of permissions we ever granted a single extension.
bool Extension::IsPrivilegeIncrease(Extension* old_extension,
                                    Extension* new_extension) {
  // If the old extension had native code access, we don't need to go any
  // further. Things can't get any worse.
  if (old_extension->plugins().size() > 0)
    return false;

  // Otherwise, if the new extension has a plugin, it's a privilege increase.
  if (new_extension->plugins().size() > 0)
    return true;

  // If we are increasing the set of hosts we have access to (not
  // counting scheme differences), it's a privilege increase.
  if (!old_extension->HasEffectiveAccessToAllHosts()) {
    if (new_extension->HasEffectiveAccessToAllHosts())
      return true;

    // TODO(erikkay) This will trip when you add a new distinct hostname,
    // but we should unique based on RCD as well.  crbug.com/57042
    std::vector<std::string> old_hosts = old_extension->GetDistinctHosts();
    std::vector<std::string> new_hosts = new_extension->GetDistinctHosts();
    std::set<std::string> old_hosts_set(old_hosts.begin(), old_hosts.end());
    std::set<std::string> new_hosts_set(new_hosts.begin(), new_hosts.end());
    std::set<std::string> new_only;
    std::set_difference(new_hosts_set.begin(), new_hosts_set.end(),
                      old_hosts_set.begin(), old_hosts_set.end(),
                      std::inserter(new_only, new_only.end()));
    if (new_only.size())
      return true;
  }

  std::set<string16> old_messages =
      old_extension->GetSimplePermissionMessages();
  std::set<string16> new_messages =
      new_extension->GetSimplePermissionMessages();
  std::set<string16> new_only;
  std::set_difference(new_messages.begin(), new_messages.end(),
                      old_messages.begin(), old_messages.end(),
                      std::inserter(new_only, new_only.end()));

  // If there are any new permission messages, then it's an increase.
  if (!new_only.empty())
    return true;

  return false;
}

// static
void Extension::DecodeIcon(Extension* extension,
                           Icons icon_size,
                           scoped_ptr<SkBitmap>* result) {
  FilePath icon_path = extension->GetIconResource(
      icon_size, ExtensionIconSet::MATCH_EXACTLY).GetFilePath();
  DecodeIconFromPath(icon_path, icon_size, result);
}

// static
void Extension::DecodeIconFromPath(const FilePath& icon_path,
                                   Icons icon_size,
                                   scoped_ptr<SkBitmap>* result) {
  ExtensionResource::CheckFileAccessFromFileThread();

  if (icon_path.empty())
    return;

  std::string file_contents;
  if (!file_util::ReadFileToString(icon_path, &file_contents)) {
    LOG(ERROR) << "Could not read icon file: "
               << WideToUTF8(icon_path.ToWStringHack());
    return;
  }

  // Decode the image using WebKit's image decoder.
  const unsigned char* data =
    reinterpret_cast<const unsigned char*>(file_contents.data());
  webkit_glue::ImageDecoder decoder;
  scoped_ptr<SkBitmap> decoded(new SkBitmap());
  *decoded = decoder.Decode(data, file_contents.length());
  if (decoded->empty()) {
    LOG(ERROR) << "Could not decode icon file: "
               << WideToUTF8(icon_path.ToWStringHack());
    return;
  }

  if (decoded->width() != icon_size || decoded->height() != icon_size) {
    LOG(ERROR) << "Icon file has unexpected size: "
               << base::IntToString(decoded->width()) << "x"
               << base::IntToString(decoded->height());
    return;
  }

  result->swap(decoded);
}

GURL Extension::GetBaseURLFromExtensionId(const std::string& extension_id) {
  return GURL(std::string(chrome::kExtensionScheme) +
              chrome::kStandardSchemeSeparator + extension_id + "/");
}

// static
bool Extension::AppsAreEnabled() {
  return !CommandLine::ForCurrentProcess()->HasSwitch(switches::kDisableApps);
}

bool Extension::InitFromValue(const DictionaryValue& source, bool require_key,
                              std::string* error) {
  // Unit tests reuse Extension objects, so we need to reset mutable_static_data
  // when we re-initialize.
  mutable_static_data_ = const_cast<StaticData*>(static_data_.get());

  if (source.HasKey(keys::kPublicKey)) {
    std::string public_key_bytes;
    if (!source.GetString(keys::kPublicKey, &public_key_) ||
        !ParsePEMKeyBytes(public_key_, &public_key_bytes) ||
        !GenerateId(public_key_bytes, &mutable_static_data_->id)) {
      *error = errors::kInvalidKey;
      return false;
    }
  } else if (require_key) {
    *error = errors::kInvalidKey;
    return false;
  } else {
    // If there is a path, we generate the ID from it. This is useful for
    // development mode, because it keeps the ID stable across restarts and
    // reloading the extension.
    if (!GenerateId(WideToUTF8(path().ToWStringHack()),
                    &mutable_static_data_->id)) {
      NOTREACHED() << "Could not create ID from path.";
      return false;
    }
  }

  // Make a copy of the manifest so we can store it in prefs.
  manifest_value_.reset(static_cast<DictionaryValue*>(source.DeepCopy()));

  // Initialize the URL.
  extension_url_ = Extension::GetBaseURLFromExtensionId(id());

  // Initialize version.
  std::string version_str;
  if (!source.GetString(keys::kVersion, &version_str)) {
    *error = errors::kInvalidVersion;
    return false;
  }
  version_.reset(Version::GetVersionFromString(version_str));
  if (!version_.get() || version_->components().size() > 4) {
    *error = errors::kInvalidVersion;
    return false;
  }

  // Initialize name.
  string16 localized_name;
  if (!source.GetString(keys::kName, &localized_name)) {
    *error = errors::kInvalidName;
    return false;
  }
  base::i18n::AdjustStringForLocaleDirection(localized_name, &localized_name);
  mutable_static_data_->name = UTF16ToUTF8(localized_name);

  // Initialize description (if present).
  if (source.HasKey(keys::kDescription)) {
    if (!source.GetString(keys::kDescription, &description_)) {
      *error = errors::kInvalidDescription;
      return false;
    }
  }

  // Initialize update url (if present).
  if (source.HasKey(keys::kUpdateURL)) {
    std::string tmp;
    if (!source.GetString(keys::kUpdateURL, &tmp)) {
      *error = ExtensionErrorUtils::FormatErrorMessage(
          errors::kInvalidUpdateURL, "");
      return false;
    }
    update_url_ = GURL(tmp);
    if (!update_url_.is_valid() || update_url_.has_ref()) {
      *error = ExtensionErrorUtils::FormatErrorMessage(
          errors::kInvalidUpdateURL, tmp);
      return false;
    }
  }

  // Validate minimum Chrome version (if present). We don't need to store this,
  // since the extension is not valid if it is incorrect.
  if (source.HasKey(keys::kMinimumChromeVersion)) {
    std::string minimum_version_string;
    if (!source.GetString(keys::kMinimumChromeVersion,
                          &minimum_version_string)) {
      *error = errors::kInvalidMinimumChromeVersion;
      return false;
    }

    scoped_ptr<Version> minimum_version(
        Version::GetVersionFromString(minimum_version_string));
    if (!minimum_version.get()) {
      *error = errors::kInvalidMinimumChromeVersion;
      return false;
    }

    chrome::VersionInfo current_version_info;
    if (!current_version_info.is_valid()) {
      NOTREACHED();
      return false;
    }

    scoped_ptr<Version> current_version(
        Version::GetVersionFromString(current_version_info.Version()));
    if (!current_version.get()) {
      DCHECK(false);
      return false;
    }

    if (current_version->CompareTo(*minimum_version) < 0) {
      *error = ExtensionErrorUtils::FormatErrorMessage(
          errors::kChromeVersionTooLow,
          l10n_util::GetStringUTF8(IDS_PRODUCT_NAME),
          minimum_version_string);
      return false;
    }
  }

  // Initialize converted_from_user_script (if present)
  source.GetBoolean(keys::kConvertedFromUserScript,
                    &converted_from_user_script_);

  // Initialize icons (if present).
  if (source.HasKey(keys::kIcons)) {
    DictionaryValue* icons_value = NULL;
    if (!source.GetDictionary(keys::kIcons, &icons_value)) {
      *error = errors::kInvalidIcons;
      return false;
    }

    for (size_t i = 0; i < arraysize(kIconSizes); ++i) {
      std::string key = base::IntToString(kIconSizes[i]);
      if (icons_value->HasKey(key)) {
        std::string icon_path;
        if (!icons_value->GetString(key, &icon_path)) {
          *error = ExtensionErrorUtils::FormatErrorMessage(
              errors::kInvalidIconPath, key);
          return false;
        }

        if (icon_path.size() > 0 && icon_path[0] == '/')
          icon_path = icon_path.substr(1);

        if (icon_path.empty()) {
          *error = ExtensionErrorUtils::FormatErrorMessage(
              errors::kInvalidIconPath, key);
          return false;
        }

        mutable_static_data_->icons.Add(kIconSizes[i], icon_path);
      }
    }
  }

  // Initialize themes (if present).
  is_theme_ = false;
  if (source.HasKey(keys::kTheme)) {
    // Themes cannot contain extension keys.
    if (ContainsNonThemeKeys(source)) {
      *error = errors::kThemesCannotContainExtensions;
      return false;
    }

    DictionaryValue* theme_value;
    if (!source.GetDictionary(keys::kTheme, &theme_value)) {
      *error = errors::kInvalidTheme;
      return false;
    }
    is_theme_ = true;

    DictionaryValue* images_value;
    if (theme_value->GetDictionary(keys::kThemeImages, &images_value)) {
      // Validate that the images are all strings
      for (DictionaryValue::key_iterator iter = images_value->begin_keys();
           iter != images_value->end_keys(); ++iter) {
        std::string val;
        if (!images_value->GetString(*iter, &val)) {
          *error = errors::kInvalidThemeImages;
          return false;
        }
      }
      theme_images_.reset(
          static_cast<DictionaryValue*>(images_value->DeepCopy()));
    }

    DictionaryValue* colors_value;
    if (theme_value->GetDictionary(keys::kThemeColors, &colors_value)) {
      // Validate that the colors are RGB or RGBA lists
      for (DictionaryValue::key_iterator iter = colors_value->begin_keys();
           iter != colors_value->end_keys(); ++iter) {
        ListValue* color_list;
        double alpha;
        int alpha_int;
        int color;
        // The color must be a list
        if (!colors_value->GetListWithoutPathExpansion(*iter, &color_list) ||
            // And either 3 items (RGB) or 4 (RGBA)
            ((color_list->GetSize() != 3) &&
             ((color_list->GetSize() != 4) ||
              // For RGBA, the fourth item must be a real or int alpha value
              (!color_list->GetReal(3, &alpha) &&
               !color_list->GetInteger(3, &alpha_int)))) ||
            // For both RGB and RGBA, the first three items must be ints (R,G,B)
            !color_list->GetInteger(0, &color) ||
            !color_list->GetInteger(1, &color) ||
            !color_list->GetInteger(2, &color)) {
          *error = errors::kInvalidThemeColors;
          return false;
        }
      }
      theme_colors_.reset(
          static_cast<DictionaryValue*>(colors_value->DeepCopy()));
    }

    DictionaryValue* tints_value;
    if (theme_value->GetDictionary(keys::kThemeTints, &tints_value)) {
      // Validate that the tints are all reals.
      for (DictionaryValue::key_iterator iter = tints_value->begin_keys();
           iter != tints_value->end_keys(); ++iter) {
        ListValue* tint_list;
        double v;
        int vi;
        if (!tints_value->GetListWithoutPathExpansion(*iter, &tint_list) ||
            tint_list->GetSize() != 3 ||
            !(tint_list->GetReal(0, &v) || tint_list->GetInteger(0, &vi)) ||
            !(tint_list->GetReal(1, &v) || tint_list->GetInteger(1, &vi)) ||
            !(tint_list->GetReal(2, &v) || tint_list->GetInteger(2, &vi))) {
          *error = errors::kInvalidThemeTints;
          return false;
        }
      }
      theme_tints_.reset(
          static_cast<DictionaryValue*>(tints_value->DeepCopy()));
    }

    DictionaryValue* display_properties_value;
    if (theme_value->GetDictionary(keys::kThemeDisplayProperties,
        &display_properties_value)) {
      theme_display_properties_.reset(
          static_cast<DictionaryValue*>(display_properties_value->DeepCopy()));
    }

    return true;
  }

  // Initialize plugins (optional).
  if (source.HasKey(keys::kPlugins)) {
    ListValue* list_value;
    if (!source.GetList(keys::kPlugins, &list_value)) {
      *error = errors::kInvalidPlugins;
      return false;
    }

#if defined(OS_CHROMEOS)
    if (list_value->GetSize() > 0) {
      *error = errors::kIllegalPlugins;
      return false;
    }
#endif

    for (size_t i = 0; i < list_value->GetSize(); ++i) {
      DictionaryValue* plugin_value;
      std::string path_str;
      bool is_public = false;

      if (!list_value->GetDictionary(i, &plugin_value)) {
        *error = errors::kInvalidPlugins;
        return false;
      }

      // Get plugins[i].path.
      if (!plugin_value->GetString(keys::kPluginsPath, &path_str)) {
        *error = ExtensionErrorUtils::FormatErrorMessage(
            errors::kInvalidPluginsPath, base::IntToString(i));
        return false;
      }

      // Get plugins[i].content (optional).
      if (plugin_value->HasKey(keys::kPluginsPublic)) {
        if (!plugin_value->GetBoolean(keys::kPluginsPublic, &is_public)) {
          *error = ExtensionErrorUtils::FormatErrorMessage(
              errors::kInvalidPluginsPublic, base::IntToString(i));
          return false;
        }
      }

      plugins_.push_back(PluginInfo());
      plugins_.back().path = path().AppendASCII(path_str);
      plugins_.back().is_public = is_public;
    }
  }

  // Initialize background url (optional).
  if (source.HasKey(keys::kBackground)) {
    std::string background_str;
    if (!source.GetString(keys::kBackground, &background_str)) {
      *error = errors::kInvalidBackground;
      return false;
    }
    background_url_ = GetResourceURL(background_str);
  }

  // Initialize toolstrips.  This is deprecated for public use.
  // NOTE(erikkay) Although deprecated, we intend to preserve this parsing
  // code indefinitely.  Please contact me or Joi for details as to why.
  if (CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableExperimentalExtensionApis) &&
      source.HasKey(keys::kToolstrips)) {
    ListValue* list_value;
    if (!source.GetList(keys::kToolstrips, &list_value)) {
      *error = errors::kInvalidToolstrips;
      return false;
    }

    for (size_t i = 0; i < list_value->GetSize(); ++i) {
      GURL toolstrip;
      DictionaryValue* toolstrip_value;
      std::string toolstrip_path;
      if (list_value->GetString(i, &toolstrip_path)) {
        // Support a simple URL value for backwards compatibility.
        toolstrip = GetResourceURL(toolstrip_path);
      } else if (list_value->GetDictionary(i, &toolstrip_value)) {
        if (!toolstrip_value->GetString(keys::kToolstripPath,
                                        &toolstrip_path)) {
          *error = ExtensionErrorUtils::FormatErrorMessage(
              errors::kInvalidToolstrip, base::IntToString(i));
          return false;
        }
        toolstrip = GetResourceURL(toolstrip_path);
      } else {
        *error = ExtensionErrorUtils::FormatErrorMessage(
            errors::kInvalidToolstrip, base::IntToString(i));
        return false;
      }
      toolstrips_.push_back(toolstrip);
    }
  }

  // Initialize content scripts (optional).
  if (source.HasKey(keys::kContentScripts)) {
    ListValue* list_value;
    if (!source.GetList(keys::kContentScripts, &list_value)) {
      *error = errors::kInvalidContentScriptsList;
      return false;
    }

    for (size_t i = 0; i < list_value->GetSize(); ++i) {
      DictionaryValue* content_script;
      if (!list_value->GetDictionary(i, &content_script)) {
        *error = ExtensionErrorUtils::FormatErrorMessage(
            errors::kInvalidContentScript, base::IntToString(i));
        return false;
      }

      UserScript script;
      if (!LoadUserScriptHelper(content_script, i, error, &script))
        return false;  // Failed to parse script context definition
      script.set_extension_id(id());
      if (converted_from_user_script_) {
        script.set_emulate_greasemonkey(true);
        script.set_match_all_frames(true);  // greasemonkey matches all frames
      }
      content_scripts_.push_back(script);
    }
  }

  // Initialize page action (optional).
  DictionaryValue* page_action_value = NULL;

  if (source.HasKey(keys::kPageActions)) {
    ListValue* list_value;
    if (!source.GetList(keys::kPageActions, &list_value)) {
      *error = errors::kInvalidPageActionsList;
      return false;
    }

    size_t list_value_length = list_value->GetSize();

    if (list_value_length == 0u) {
      // A list with zero items is allowed, and is equivalent to not having
      // a page_actions key in the manifest.  Don't set |page_action_value|.
    } else if (list_value_length == 1u) {
      if (!list_value->GetDictionary(0, &page_action_value)) {
        *error = errors::kInvalidPageAction;
        return false;
      }
    } else {  // list_value_length > 1u.
      *error = errors::kInvalidPageActionsListSize;
      return false;
    }
  } else if (source.HasKey(keys::kPageAction)) {
    if (!source.GetDictionary(keys::kPageAction, &page_action_value)) {
      *error = errors::kInvalidPageAction;
      return false;
    }
  }

  // If page_action_value is not NULL, then there was a valid page action.
  if (page_action_value) {
    page_action_.reset(
        LoadExtensionActionHelper(page_action_value, error));
    if (!page_action_.get())
      return false;  // Failed to parse page action definition.
  }

  // Initialize browser action (optional).
  if (source.HasKey(keys::kBrowserAction)) {
    // Restrict extensions to one UI surface.
    if (page_action_.get()) {
      *error = errors::kOneUISurfaceOnly;
      return false;
    }

    DictionaryValue* browser_action_value;
    if (!source.GetDictionary(keys::kBrowserAction, &browser_action_value)) {
      *error = errors::kInvalidBrowserAction;
      return false;
    }

    browser_action_.reset(
        LoadExtensionActionHelper(browser_action_value, error));
    if (!browser_action_.get())
      return false;  // Failed to parse browser action definition.
  }

  // Load App settings.
  if (!LoadIsApp(manifest_value_.get(), error) ||
      !LoadExtent(manifest_value_.get(), keys::kWebURLs,
                  &mutable_static_data_->extent,
                  errors::kInvalidWebURLs, errors::kInvalidWebURL, error) ||
      !EnsureNotHybridApp(manifest_value_.get(), error) ||
      !LoadLaunchURL(manifest_value_.get(), error) ||
      !LoadLaunchContainer(manifest_value_.get(), error)) {
    return false;
  }

  // Initialize options page url (optional).
  // Funtion LoadIsApp() set is_app_ above.
  if (source.HasKey(keys::kOptionsPage)) {
    std::string options_str;
    if (!source.GetString(keys::kOptionsPage, &options_str)) {
      *error = errors::kInvalidOptionsPage;
      return false;
    }

    if (is_hosted_app()) {
      // hosted apps require an absolute URL.
      GURL options_url(options_str);
      if (!options_url.is_valid() ||
          !(options_url.SchemeIs("http") || options_url.SchemeIs("https"))) {
        *error = errors::kInvalidOptionsPageInHostedApp;
        return false;
      }
      options_url_ = options_url;

    } else {
      GURL absolute(options_str);
      if (absolute.is_valid()) {
        *error = errors::kInvalidOptionsPageExpectUrlInPackage;
        return false;
      }
      options_url_ = GetResourceURL(options_str);
      if (!options_url_.is_valid()) {
        *error = errors::kInvalidOptionsPage;
        return false;
      }
    }
  }

  // Initialize the permissions (optional).
  if (source.HasKey(keys::kPermissions)) {
    ListValue* permissions = NULL;
    if (!source.GetList(keys::kPermissions, &permissions)) {
      *error = ExtensionErrorUtils::FormatErrorMessage(
          errors::kInvalidPermissions, "");
      return false;
    }

    for (size_t i = 0; i < permissions->GetSize(); ++i) {
      std::string permission_str;
      if (!permissions->GetString(i, &permission_str)) {
        *error = ExtensionErrorUtils::FormatErrorMessage(
            errors::kInvalidPermission, base::IntToString(i));
        return false;
      }

      // Only COMPONENT extensions can use the webstorePrivate APIs.
      // TODO(asargent) - We want a more general purpose mechanism for this,
      // and better error messages. (http://crbug.com/54013)
      if (permission_str == kWebstorePrivatePermission &&
          location_ != Extension::COMPONENT) {
        continue;
      }

      // Remap the old unlimited storage permission name.
      if (permission_str == kOldUnlimitedStoragePermission)
        permission_str = kUnlimitedStoragePermission;

      if (web_extent().is_empty() || location() == Extension::COMPONENT) {
        // Check if it's a module permission.  If so, enable that permission.
        if (IsAPIPermission(permission_str)) {
          mutable_static_data_->api_permissions.insert(permission_str);
          continue;
        }
      } else {
        // Hosted apps only get access to a subset of the valid permissions.
        if (IsHostedAppPermission(permission_str)) {
          mutable_static_data_->api_permissions.insert(permission_str);
          continue;
        }
      }

      // Otherwise, it's a host pattern permission.
      URLPattern pattern(URLPattern::SCHEME_HTTP |
                         URLPattern::SCHEME_HTTPS |
                         URLPattern::SCHEME_CHROMEUI);
      if (!pattern.Parse(permission_str)) {
        *error = ExtensionErrorUtils::FormatErrorMessage(
            errors::kInvalidPermission, base::IntToString(i));
        return false;
      }

      if (!CanAccessURL(pattern)) {
        *error = ExtensionErrorUtils::FormatErrorMessage(
            errors::kInvalidPermissionScheme, base::IntToString(i));
        return false;
      }

      // The path component is not used for host permissions, so we force it to
      // match all paths.
      pattern.set_path("/*");

      host_permissions_.push_back(pattern);
    }
  }

  if (source.HasKey(keys::kDefaultLocale)) {
    if (!source.GetString(keys::kDefaultLocale,
                          &mutable_static_data_->default_locale) ||
        mutable_static_data_->default_locale.empty()) {
      *error = errors::kInvalidDefaultLocale;
      return false;
    }
  }

  // Chrome URL overrides (optional)
  if (source.HasKey(keys::kChromeURLOverrides)) {
    DictionaryValue* overrides;
    if (!source.GetDictionary(keys::kChromeURLOverrides, &overrides)) {
      *error = errors::kInvalidChromeURLOverrides;
      return false;
    }

    // Validate that the overrides are all strings
    for (DictionaryValue::key_iterator iter = overrides->begin_keys();
         iter != overrides->end_keys(); ++iter) {
      std::string page = *iter;
      std::string val;
      // Restrict override pages to a list of supported URLs.
      if ((page != chrome::kChromeUINewTabHost &&
#if defined(TOUCH_UI)
           page != chrome::kChromeUIKeyboardHost &&
#endif
           page != chrome::kChromeUIBookmarksHost &&
           page != chrome::kChromeUIHistoryHost) ||
          !overrides->GetStringWithoutPathExpansion(*iter, &val)) {
        *error = errors::kInvalidChromeURLOverrides;
        return false;
      }
      // Replace the entry with a fully qualified chrome-extension:// URL.
      chrome_url_overrides_[page] = GetResourceURL(val);
    }

    // An extension may override at most one page.
    if (overrides->size() > 1) {
      *error = errors::kMultipleOverrides;
      return false;
    }
  }

  if (source.HasKey(keys::kOmniboxKeyword)) {
    if (!source.GetString(keys::kOmniboxKeyword, &omnibox_keyword_) ||
        omnibox_keyword_.empty()) {
      *error = errors::kInvalidOmniboxKeyword;
      return false;
    }
    if (!HasApiPermission(Extension::kExperimentalPermission)) {
      *error = errors::kOmniboxExperimental;
      return false;
    }
  }

  // Initialize devtools page url (optional).
  if (source.HasKey(keys::kDevToolsPage)) {
    std::string devtools_str;
    if (!source.GetString(keys::kDevToolsPage, &devtools_str)) {
      *error = errors::kInvalidDevToolsPage;
      return false;
    }
    if (!HasApiPermission(Extension::kExperimentalPermission)) {
      *error = errors::kDevToolsExperimental;
      return false;
    }
    devtools_url_ = GetResourceURL(devtools_str);
  }

  // Initialize incognito behavior. Apps default to split mode, extensions
  // default to spanning.
  mutable_static_data_->incognito_split_mode = is_app_;
  if (source.HasKey(keys::kIncognito)) {
    std::string value;
    if (!source.GetString(keys::kIncognito, &value)) {
      *error = errors::kInvalidIncognitoBehavior;
      return false;
    }
    if (value == values::kIncognitoSpanning) {
      mutable_static_data_->incognito_split_mode = false;
    } else if (value == values::kIncognitoSplit) {
      mutable_static_data_->incognito_split_mode = true;
    } else {
      *error = errors::kInvalidIncognitoBehavior;
      return false;
    }
  }

  InitEffectiveHostPermissions();

  // Although |source| is passed in as a const, it's still possible to modify
  // it.  This is dangerous since the utility process re-uses |source| after
  // it calls InitFromValue, passing it up to the browser process which calls
  // InitFromValue again.  As a result, we need to make sure that nobody
  // accidentally modifies it.
  DCHECK(source.Equals(manifest_value_.get()));

  // Ensure we can't modify our static data anymore.
  mutable_static_data_ = NULL;

  return true;
}

// static
std::string Extension::ChromeStoreURL() {
  std::string gallery_prefix = extension_urls::kGalleryBrowsePrefix;
  if (CommandLine::ForCurrentProcess()->HasSwitch(switches::kAppsGalleryURL))
    gallery_prefix = CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
        switches::kAppsGalleryURL);
  if (EndsWith(gallery_prefix, "/", true))
    gallery_prefix = gallery_prefix.substr(0, gallery_prefix.length() - 1);
  return gallery_prefix;
}

GURL Extension::GalleryUrl() const {
  if (!update_url_.DomainIs("google.com"))
    return GURL();

  GURL url(ChromeStoreURL() + std::string("/detail/") + id());

  return url;
}

std::set<FilePath> Extension::GetBrowserImages() {
  std::set<FilePath> image_paths;
  // TODO(viettrungluu): These |FilePath::FromWStringHack(UTF8ToWide())|
  // indicate that we're doing something wrong.

  // Extension icons.
  for (ExtensionIconSet::IconMap::const_iterator iter = icons().map().begin();
       iter != icons().map().end(); ++iter) {
    image_paths.insert(FilePath::FromWStringHack(UTF8ToWide(iter->second)));
  }

  // Theme images.
  DictionaryValue* theme_images = GetThemeImages();
  if (theme_images) {
    for (DictionaryValue::key_iterator it = theme_images->begin_keys();
         it != theme_images->end_keys(); ++it) {
      std::string val;
      if (theme_images->GetStringWithoutPathExpansion(*it, &val))
        image_paths.insert(FilePath::FromWStringHack(UTF8ToWide(val)));
    }
  }

  // Page action icons.
  if (page_action_.get()) {
    std::vector<std::string>* icon_paths = page_action_->icon_paths();
    for (std::vector<std::string>::iterator iter = icon_paths->begin();
         iter != icon_paths->end(); ++iter) {
      image_paths.insert(FilePath::FromWStringHack(UTF8ToWide(*iter)));
    }
  }

  // Browser action icons.
  if (browser_action_.get()) {
    std::vector<std::string>* icon_paths = browser_action_->icon_paths();
    for (std::vector<std::string>::iterator iter = icon_paths->begin();
         iter != icon_paths->end(); ++iter) {
      image_paths.insert(FilePath::FromWStringHack(UTF8ToWide(*iter)));
    }
  }

  return image_paths;
}

GURL Extension::GetFullLaunchURL() const {
  if (!launch_local_path_.empty())
    return extension_url_.Resolve(launch_local_path_);
  else
    return GURL(launch_web_url_);
}

bool Extension::GetBackgroundPageReady() {
  return background_page_ready_ || background_url().is_empty();
}

void Extension::SetBackgroundPageReady() {
  DCHECK(!background_url().is_empty());
  background_page_ready_ = true;
  NotificationService::current()->Notify(
      NotificationType::EXTENSION_BACKGROUND_PAGE_READY,
      Source<Extension>(this),
      NotificationService::NoDetails());
}

static std::string SizeToString(const gfx::Size& max_size) {
  return base::IntToString(max_size.width()) + "x" +
         base::IntToString(max_size.height());
}

void Extension::SetCachedImage(const ExtensionResource& source,
                               const SkBitmap& image,
                               const gfx::Size& original_size) {
  DCHECK(source.extension_root() == path());  // The resource must come from
                                              // this extension.
  const FilePath& path = source.relative_path();
  gfx::Size actual_size(image.width(), image.height());
  if (actual_size == original_size) {
    image_cache_[ImageCacheKey(path, std::string())] = image;
  } else {
    image_cache_[ImageCacheKey(path, SizeToString(actual_size))] = image;
  }
}

bool Extension::HasCachedImage(const ExtensionResource& source,
                               const gfx::Size& max_size) {
  DCHECK(source.extension_root() == path());  // The resource must come from
                                              // this extension.
  return GetCachedImageImpl(source, max_size) != NULL;
}

SkBitmap Extension::GetCachedImage(const ExtensionResource& source,
                                   const gfx::Size& max_size) {
  DCHECK(source.extension_root() == path());  // The resource must come from
                                              // this extension.
  SkBitmap* image = GetCachedImageImpl(source, max_size);
  return image ? *image : SkBitmap();
}

SkBitmap* Extension::GetCachedImageImpl(const ExtensionResource& source,
                                        const gfx::Size& max_size) {
  const FilePath& path = source.relative_path();

  // Look for exact size match.
  ImageCache::iterator i = image_cache_.find(
      ImageCacheKey(path, SizeToString(max_size)));
  if (i != image_cache_.end())
    return &(i->second);

  // If we have the original size version cached, return that if it's small
  // enough.
  i = image_cache_.find(ImageCacheKey(path, std::string()));
  if (i != image_cache_.end()) {
    SkBitmap& image = i->second;
    if (image.width() <= max_size.width() &&
        image.height() <= max_size.height())
      return &(i->second);
  }

  return NULL;
}

ExtensionResource Extension::GetIconResource(
    int size, ExtensionIconSet::MatchType match_type) {
  std::string path = icons().Get(size, match_type);
  if (path.empty())
    return ExtensionResource();
  return GetResource(path);
}

GURL Extension::GetIconURL(int size, ExtensionIconSet::MatchType match_type) {
  std::string path = icons().Get(size, match_type);
  if (path.empty())
    return GURL();
  else
    return GetResourceURL(path);
}

bool Extension::CanAccessURL(const URLPattern pattern) const {
  if (pattern.MatchesScheme(chrome::kChromeUIScheme)) {
    // Only allow access to chrome://favicon to regular extensions. Component
    // extensions can have access to all of chrome://*.
    return (pattern.host() == chrome::kChromeUIFavIconHost ||
            location() == Extension::COMPONENT);
  }

  // Otherwise, the valid schemes were handled by URLPattern.
  return true;
}

// static.
bool Extension::HasApiPermission(
    const std::set<std::string>& api_permissions,
    const std::string& function_name) {
  std::string permission_name = function_name;

  for (size_t i = 0; i < kNumNonPermissionFunctionNames; ++i) {
    if (permission_name == kNonPermissionFunctionNames[i])
      return true;
  }

  // See if this is a function or event name first and strip out the package.
  // Functions will be of the form package.function
  // Events will be of the form package/id or package.optional.stuff
  size_t separator = function_name.find_first_of("./");
  if (separator != std::string::npos)
    permission_name = function_name.substr(0, separator);

  // windows and tabs are the same permission.
  if (permission_name == kWindowPermission)
    permission_name = Extension::kTabPermission;

  if (api_permissions.count(permission_name))
    return true;

  for (size_t i = 0; i < kNumNonPermissionModuleNames; ++i) {
    if (permission_name == kNonPermissionModuleNames[i]) {
      return true;
    }
  }

  return false;
}

bool Extension::HasHostPermission(const GURL& url) const {
  for (URLPatternList::const_iterator host = host_permissions_.begin();
       host != host_permissions_.end(); ++host) {
    if (host->MatchesUrl(url))
      return true;
  }
  return false;
}

void Extension::InitEffectiveHostPermissions() {
  for (URLPatternList::const_iterator host = host_permissions_.begin();
       host != host_permissions_.end(); ++host)
    mutable_static_data_->effective_host_permissions.AddPattern(*host);

  for (UserScriptList::const_iterator content_script = content_scripts_.begin();
       content_script != content_scripts_.end(); ++content_script) {
    UserScript::PatternList::const_iterator pattern =
        content_script->url_patterns().begin();
    for (; pattern != content_script->url_patterns().end(); ++pattern)
      mutable_static_data_->effective_host_permissions.AddPattern(*pattern);
  }
}

bool Extension::HasEffectiveAccessToAllHosts() const {
  // Some APIs effectively grant access to every site.  New ones should be
  // added here.  (I'm looking at you, network API)
  if (HasApiPermission(kProxyPermission))
    return true;

  for (URLPatternList::const_iterator host = host_permissions_.begin();
       host != host_permissions_.end(); ++host) {
    if (host->match_subdomains() && host->host().empty())
      return true;
  }

  for (UserScriptList::const_iterator content_script = content_scripts_.begin();
       content_script != content_scripts_.end(); ++content_script) {
    UserScript::PatternList::const_iterator pattern =
        content_script->url_patterns().begin();
    for (; pattern != content_script->url_patterns().end(); ++pattern) {
      if (pattern->match_subdomains() && pattern->host().empty())
        return true;
    }
  }

  return false;
}

bool Extension::IsAPIPermission(const std::string& str) {
  for (size_t i = 0; i < Extension::kNumPermissions; ++i) {
    if (str == Extension::kPermissions[i].name) {
      // Only allow the experimental API permission if the command line
      // flag is present, or if the extension is a component of Chrome.
      if (str == Extension::kExperimentalPermission) {
        if (CommandLine::ForCurrentProcess()->HasSwitch(
                switches::kEnableExperimentalExtensionApis)) {
          return true;
        } else if (location() == Extension::COMPONENT) {
          return true;
        } else {
          return false;
        }
      } else {
        return true;
      }
    }
  }
  return false;
}

ExtensionInfo::ExtensionInfo(const DictionaryValue* manifest,
                             const std::string& id,
                             const FilePath& path,
                             Extension::Location location)
    : extension_id(id),
      extension_path(path),
      extension_location(location) {
  if (manifest)
    extension_manifest.reset(
        static_cast<DictionaryValue*>(manifest->DeepCopy()));
}

ExtensionInfo::~ExtensionInfo() {
}

UninstalledExtensionInfo::UninstalledExtensionInfo(
    const Extension& extension)
    : extension_id(extension.id()),
      extension_api_permissions(extension.api_permissions()),
      is_theme(extension.is_theme()),
      is_app(extension.is_app()),
      converted_from_user_script(extension.converted_from_user_script()),
      update_url(extension.update_url()) {}
