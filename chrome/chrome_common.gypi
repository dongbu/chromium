# Copyright (c) 2009 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

{
  'targets': [
    {
      'target_name': 'common',
      'type': '<(library)',
      'msvs_guid': '899F1280-3441-4D1F-BA04-CCD6208D9146',
      'dependencies': [
        'common_constants',
        'chrome_resources',
        'chrome_strings',
        'theme_resources',
        '../app/app.gyp:app_base',
        '../app/app.gyp:app_resources',
        '../base/base.gyp:base',
        '../base/base.gyp:base_i18n',
        '../build/temp_gyp/googleurl.gyp:googleurl',
        '../ipc/ipc.gyp:ipc',
        '../net/net.gyp:net',
        '../net/net.gyp:net_resources',
        '../skia/skia.gyp:skia',
        '../third_party/icu/icu.gyp:icui18n',
        '../third_party/icu/icu.gyp:icuuc',
        '../third_party/libxml/libxml.gyp:libxml',
        '../third_party/sqlite/sqlite.gyp:sqlite',
        '../third_party/zlib/zlib.gyp:zlib',
        '../third_party/npapi/npapi.gyp:npapi',
        '../webkit/webkit.gyp:appcache',
        '../webkit/webkit.gyp:glue',
      ],
      'include_dirs': [
        '..',
      ],
      'sources': [
        # All .cc, .h, and .mm files under chrome/common except for tests.
        'common/desktop_notifications/active_notification_tracker.h',
        'common/desktop_notifications/active_notification_tracker.cc',
        'common/extensions/extension.cc',
        'common/extensions/extension.h',
        'common/extensions/extension_constants.cc',
        'common/extensions/extension_constants.h',
        'common/extensions/extension_error_reporter.cc',
        'common/extensions/extension_error_reporter.h',
        'common/extensions/extension_error_utils.cc',
        'common/extensions/extension_error_utils.h',
        'common/extensions/extension_action.cc',
        'common/extensions/extension_action.h',
        'common/extensions/extension_l10n_util.cc',
        'common/extensions/extension_l10n_util.h',
        'common/extensions/extension_message_bundle.cc',
        'common/extensions/extension_message_bundle.h',
        'common/extensions/extension_resource.cc',
        'common/extensions/extension_resource.h',
        'common/extensions/extension_unpacker.cc',
        'common/extensions/extension_unpacker.h',
        'common/extensions/update_manifest.cc',
        'common/extensions/update_manifest.h',
        'common/extensions/url_pattern.cc',
        'common/extensions/url_pattern.h',
        'common/extensions/user_script.cc',
        'common/extensions/user_script.h',
        'common/gfx/utils.h',
        'common/net/dns.h',
        'common/net/net_resource_provider.cc',
        'common/net/net_resource_provider.h',
        'common/net/socket_stream.h',
        'common/net/url_request_intercept_job.cc',
        'common/net/url_request_intercept_job.h',
        'common/web_resource/web_resource_unpacker.cc',
        'common/web_resource/web_resource_unpacker.h',
        'common/appcache/appcache_backend_proxy.cc',
        'common/appcache/appcache_backend_proxy.h',
        'common/appcache/appcache_dispatcher.cc',
        'common/appcache/appcache_dispatcher.h',
        'common/appcache/appcache_dispatcher_host.cc',
        'common/appcache/appcache_dispatcher_host.h',
        'common/appcache/appcache_frontend_proxy.cc',
        'common/appcache/appcache_frontend_proxy.h',
        'common/appcache/chrome_appcache_service.h',
        'common/automation_constants.cc',
        'common/automation_constants.h',
        'common/bindings_policy.h',
        'common/child_process.cc',
        'common/child_process.h',
        'common/child_process_host.cc',
        'common/child_process_host.h',
        'common/child_process_info.cc',
        'common/child_process_info.h',
        'common/child_process_logging.h',
        'common/child_process_logging_linux.cc',
        'common/child_process_logging_mac.mm',
        'common/child_process_logging_win.cc',
        'common/child_thread.cc',
        'common/child_thread.h',
        'common/chrome_counters.cc',
        'common/chrome_counters.h',
        'common/chrome_descriptors.h',
        'common/chrome_plugin_api.h',
        'common/chrome_plugin_lib.cc',
        'common/chrome_plugin_lib.h',
        'common/chrome_plugin_util.cc',
        'common/chrome_plugin_util.h',
        'common/common_glue.cc',
        'common/common_param_traits.cc',
        'common/common_param_traits.h',
        'common/css_colors.h',
        'common/db_message_filter.cc',
        'common/db_message_filter.h',
        'common/debug_flags.cc',
        'common/debug_flags.h',
        'common/devtools_messages.h',
        'common/devtools_messages_internal.h',
        'common/dom_storage_type.h',
        'common/filter_policy.h',
        'common/gears_api.h',
        'common/gtk_tree.cc',
        'common/gtk_tree.h',
        'common/gtk_util.cc',
        'common/gtk_util.h',
        'common/histogram_synchronizer.cc',
        'common/histogram_synchronizer.h',
        'common/important_file_writer.cc',
        'common/important_file_writer.h',
        'common/jstemplate_builder.cc',
        'common/jstemplate_builder.h',
        'common/libxml_utils.cc',
        'common/libxml_utils.h',
        'common/logging_chrome.cc',
        'common/logging_chrome.h',
        'common/main_function_params.h',
        'common/message_router.cc',
        'common/message_router.h',
        'common/mru_cache.h',
        'common/navigation_gesture.h',
        'common/navigation_types.h',
        'common/native_web_keyboard_event.h',
        'common/native_web_keyboard_event_linux.cc',
        'common/native_web_keyboard_event_mac.mm',
        'common/native_web_keyboard_event_win.cc',
        'common/native_window_notification_source.h',
        'common/notification_details.h',
        'common/notification_observer.h',
        'common/notification_registrar.cc',
        'common/notification_registrar.h',
        'common/notification_service.cc',
        'common/notification_service.h',
        'common/notification_source.h',
        'common/notification_type.h',
        'common/owned_widget_gtk.cc',
        'common/owned_widget_gtk.h',
        'common/page_transition_types.h',
        'common/page_zoom.h',
        'common/platform_util.h',
        'common/platform_util_linux.cc',
        'common/platform_util_mac.mm',
        'common/platform_util_win.cc',
        'common/plugin_carbon_interpose_constants_mac.h',
        'common/plugin_carbon_interpose_constants_mac.cc',
        'common/plugin_messages.h',
        'common/plugin_messages_internal.h',
        'common/pref_member.cc',
        'common/pref_member.h',
        'common/pref_service.cc',
        'common/pref_service.h',
        'common/process_watcher_posix.cc',
        'common/process_watcher_win.cc',
        'common/process_watcher.h',
        'common/property_bag.cc',
        'common/property_bag.h',
        'common/ref_counted_util.h',
        'common/render_messages.h',
        'common/render_messages_internal.h',
        'common/renderer_preferences.h',
        'common/resource_dispatcher.cc',
        'common/resource_dispatcher.h',
        'common/result_codes.h',
        'common/sandbox_init_wrapper.h',
        'common/sandbox_init_wrapper_linux.cc',
        'common/sandbox_init_wrapper_mac.cc',
        'common/sandbox_init_wrapper_win.cc',
        'common/sandbox_mac.h',
        'common/sandbox_mac.mm',
        'common/security_filter_peer.cc',
        'common/security_filter_peer.h',
        'common/nacl_messages.h',
        'common/nacl_messages_internal.h',
        'common/sqlite_compiled_statement.cc',
        'common/sqlite_compiled_statement.h',
        'common/sqlite_utils.cc',
        'common/sqlite_utils.h',
        'common/task_queue.cc',
        'common/task_queue.h',
        'common/temp_scaffolding_stubs.cc',
        'common/temp_scaffolding_stubs.h',
        'common/thumbnail_score.cc',
        'common/thumbnail_score.h',
        'common/time_format.cc',
        'common/time_format.h',
        'common/transport_dib.h',
        'common/transport_dib_linux.cc',
        'common/transport_dib_mac.cc',
        'common/transport_dib_win.cc',
        'common/url_constants.cc',
        'common/url_constants.h',
        'common/view_types.cc',
        'common/view_types.h',
        'common/visitedlink_common.cc',
        'common/visitedlink_common.h',
        'common/webkit_param_traits.h',
        'common/webmessageportchannel_impl.cc',
        'common/webmessageportchannel_impl.h',
        'common/win_safe_util.cc',
        'common/win_safe_util.h',
        'common/worker_messages.h',
        'common/worker_messages_internal.h',
        'common/worker_thread_ticker.cc',
        'common/worker_thread_ticker.h',
        'common/x11_util.cc',
        'common/x11_util.h',
        'common/x11_util_internal.h',
        'common/zip.cc',  # Requires zlib directly.
        'common/zip.h',
        'third_party/xdg_user_dirs/xdg_user_dir_lookup.cc',
      ],
      'direct_dependent_settings': {
        'include_dirs': [
          '..',
        ],
      },
      'export_dependent_settings': [
        '../app/app.gyp:app_base',
      ],
      'conditions': [
        ['OS=="linux"', {
          'dependencies': [
            '../build/linux/system.gyp:gtk',
          ],
          'export_dependent_settings': [
            '../third_party/sqlite/sqlite.gyp:sqlite',
          ],
          'link_settings': {
            'libraries': [
              '-lX11',
              '-lXrender',
              '-lXext',
            ],
          },
        }, { # else: 'OS!="linux"'
          'sources!': [
            'third_party/xdg_user_dirs/xdg_user_dir_lookup.cc',
          ],
        }],
        ['OS=="linux" and selinux==1', {
          'dependencies': [
            '../build/linux/system.gyp:selinux',
          ],
        }],
        ['OS=="win"', {
          'include_dirs': [
            'third_party/wtl/include',
          ],
        }, { # else: OS != "win"
          'sources!': [
            'common/temp_scaffolding_stubs.h',
          ],
        }],
        ['OS=="win" or OS=="linux"', {
          'sources!': [
            'common/temp_scaffolding_stubs.cc',
          ],
        }],
      ],
    },
  ],
}

# Local Variables:
# tab-width:2
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=2 shiftwidth=2:
