// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "config.h"

#include <string>

#include "ContextMenuController.h"
#include "ContextMenuItem.h"
#include "Document.h"
#include "DOMWindow.h"
#include "Event.h"
#include "Frame.h"
#include "InspectorBackend.h"
#include "InspectorController.h"
#include "InspectorFrontendHost.h"
#include "Node.h"
#include "Page.h"
#include "PlatformString.h"
#include "SecurityOrigin.h"
#include "Settings.h"
#include "V8Binding.h"
#include "V8CustomBinding.h"
#include "V8DOMWrapper.h"
#include "V8Proxy.h"
#include "V8Utilities.h"
#include <wtf/OwnPtr.h>
#include <wtf/Vector.h>
#undef LOG

#include "third_party/WebKit/WebKit/chromium/public/WebDevToolsFrontendClient.h"
#include "third_party/WebKit/WebKit/chromium/public/WebFrame.h"
#include "third_party/WebKit/WebKit/chromium/public/WebScriptSource.h"
#include "third_party/WebKit/WebKit/chromium/src/WebFrameImpl.h"
#include "third_party/WebKit/WebKit/chromium/src/WebViewImpl.h"
#include "webkit/glue/devtools/bound_object.h"
#include "webkit/glue/devtools/debugger_agent.h"
#include "webkit/glue/devtools/devtools_rpc_js.h"
#include "webkit/glue/devtools/profiler_agent.h"
#include "webkit/glue/devtools/tools_agent.h"
#include "webkit/glue/glue_util.h"
#include "webkit/glue/webdevtoolsfrontend_impl.h"

using namespace WebCore;
using WebKit::WebDevToolsFrontend;
using WebKit::WebDevToolsFrontendClient;
using WebKit::WebFrame;
using WebKit::WebFrameImpl;
using WebKit::WebScriptSource;
using WebKit::WebString;
using WebKit::WebView;
using WebKit::WebViewImpl;

static v8::Local<v8::String> ToV8String(const String& s) {
  if (s.isNull())
    return v8::Local<v8::String>();

  return v8::String::New(reinterpret_cast<const uint16_t*>(s.characters()),
                         s.length());
}

DEFINE_RPC_JS_BOUND_OBJ(DebuggerAgent, DEBUGGER_AGENT_STRUCT,
    DebuggerAgentDelegate, DEBUGGER_AGENT_DELEGATE_STRUCT)
DEFINE_RPC_JS_BOUND_OBJ(ProfilerAgent, PROFILER_AGENT_STRUCT,
    ProfilerAgentDelegate, PROFILER_AGENT_DELEGATE_STRUCT)
DEFINE_RPC_JS_BOUND_OBJ(ToolsAgent, TOOLS_AGENT_STRUCT,
    ToolsAgentDelegate, TOOLS_AGENT_DELEGATE_STRUCT)

class ToolsAgentNativeDelegateImpl : public ToolsAgentNativeDelegate {
 public:
  struct ResourceContentRequestData {
    String mime_type;
    RefPtr<Node> frame;
  };

  ToolsAgentNativeDelegateImpl(WebFrameImpl* frame) : frame_(frame) {}
  virtual ~ToolsAgentNativeDelegateImpl() {}

  // ToolsAgentNativeDelegate implementation.
  virtual void DidGetResourceContent(int request_id, const String& content) {
    if (!resource_content_requests_.contains(request_id)) {
      ASSERT_NOT_REACHED();
      return;
    }
    ResourceContentRequestData request =
        resource_content_requests_.take(request_id);

    InspectorController* ic = frame_->frame()->page()->inspectorController();
    if (request.frame && request.frame->attached()) {
      ic->inspectorFrontendHost()->addSourceToFrame(request.mime_type,
                                                    content,
                                                    request.frame.get());
    }
  }

  bool WaitingForResponse(int resource_id, Node* frame) {
    if (resource_content_requests_.contains(resource_id)) {
      ASSERT(resource_content_requests_.get(resource_id).frame.get() == frame);
      return true;
    }
    return false;
  }

  void RequestSent(int resource_id, String mime_type, Node* frame) {
    ResourceContentRequestData data;
    data.mime_type = mime_type;
    data.frame = frame;
    ASSERT(!resource_content_requests_.contains(resource_id));
    resource_content_requests_.set(resource_id, data);
  }

 private:
  WebFrameImpl* frame_;
  HashMap<int, ResourceContentRequestData> resource_content_requests_;
  DISALLOW_COPY_AND_ASSIGN(ToolsAgentNativeDelegateImpl);
};

// static
WebDevToolsFrontend* WebDevToolsFrontend::create(
    WebView* view,
    WebDevToolsFrontendClient* client,
    const WebString& application_locale) {
  return new WebDevToolsFrontendImpl(
      static_cast<WebViewImpl*>(view),
      client,
      webkit_glue::WebStringToString(application_locale));
}

WebDevToolsFrontendImpl::WebDevToolsFrontendImpl(
    WebViewImpl* web_view_impl,
    WebDevToolsFrontendClient* client,
    const String& application_locale)
    : web_view_impl_(web_view_impl),
      client_(client),
      application_locale_(application_locale),
      loaded_(false) {
  menu_selection_handler_ = MenuSelectionHandler::create(this);

  WebFrameImpl* frame = web_view_impl_->mainFrameImpl();
  v8::HandleScope scope;
  v8::Handle<v8::Context> frame_context = V8Proxy::context(frame->frame());

  debugger_agent_obj_.set(new JsDebuggerAgentBoundObj(
      this, frame_context, "RemoteDebuggerAgent"));
  profiler_agent_obj_.set(new JsProfilerAgentBoundObj(
      this, frame_context, "RemoteProfilerAgent"));
  tools_agent_obj_.set(
      new JsToolsAgentBoundObj(this, frame_context, "RemoteToolsAgent"));

  // Debugger commands should be sent using special method.
  BoundObject debugger_command_executor_obj(frame_context, this,
                                            "RemoteDebuggerCommandExecutor");
  debugger_command_executor_obj.AddProtoFunction(
      "DebuggerCommand",
      WebDevToolsFrontendImpl::JsDebuggerCommand);
  debugger_command_executor_obj.AddProtoFunction(
      "DebuggerPauseScript",
      WebDevToolsFrontendImpl::JsDebuggerPauseScript);
  debugger_command_executor_obj.Build();

  BoundObject dev_tools_host(frame_context, this, "InspectorFrontendHost");
  dev_tools_host.AddProtoFunction(
      "reset",
      WebDevToolsFrontendImpl::JsReset);
  dev_tools_host.AddProtoFunction(
      "addSourceToFrame",
      WebDevToolsFrontendImpl::JsAddSourceToFrame);
  dev_tools_host.AddProtoFunction(
      "addResourceSourceToFrame",
      WebDevToolsFrontendImpl::JsAddResourceSourceToFrame);
  dev_tools_host.AddProtoFunction(
      "loaded",
      WebDevToolsFrontendImpl::JsLoaded);
  dev_tools_host.AddProtoFunction(
      "search",
      WebCore::V8Custom::v8InspectorFrontendHostSearchCallback);
  dev_tools_host.AddProtoFunction(
      "platform",
      WebDevToolsFrontendImpl::JsPlatform);
  dev_tools_host.AddProtoFunction(
      "port",
      WebDevToolsFrontendImpl::JsPort);
  dev_tools_host.AddProtoFunction(
      "activateWindow",
      WebDevToolsFrontendImpl::JsActivateWindow);
  dev_tools_host.AddProtoFunction(
      "closeWindow",
      WebDevToolsFrontendImpl::JsCloseWindow);
  dev_tools_host.AddProtoFunction(
      "attach",
      WebDevToolsFrontendImpl::JsDockWindow);
  dev_tools_host.AddProtoFunction(
      "detach",
      WebDevToolsFrontendImpl::JsUndockWindow);
  dev_tools_host.AddProtoFunction(
      "localizedStringsURL",
      WebDevToolsFrontendImpl::JsLocalizedStringsURL);
  dev_tools_host.AddProtoFunction(
      "hiddenPanels",
      WebDevToolsFrontendImpl::JsHiddenPanels);
  dev_tools_host.AddProtoFunction(
      "setting",
      WebDevToolsFrontendImpl::JsSetting);
  dev_tools_host.AddProtoFunction(
      "setSetting",
      WebDevToolsFrontendImpl::JsSetSetting);
  dev_tools_host.AddProtoFunction(
      "windowUnloading",
      WebDevToolsFrontendImpl::JsWindowUnloading);
  dev_tools_host.AddProtoFunction(
      "showContextMenu",
      WebDevToolsFrontendImpl::JsShowContextMenu);
  dev_tools_host.Build();
}

WebDevToolsFrontendImpl::~WebDevToolsFrontendImpl() {
  menu_selection_handler_->disconnect();
}

void WebDevToolsFrontendImpl::dispatchMessageFromAgent(
      const WebString& class_name,
      const WebString& method_name,
      const WebString& param1,
      const WebString& param2,
      const WebString& param3) {
  if (ToolsAgentNativeDelegateDispatch::Dispatch(
          tools_agent_native_delegate_impl_.get(),
          webkit_glue::WebStringToString(class_name),
          webkit_glue::WebStringToString(method_name),
          webkit_glue::WebStringToString(param1),
          webkit_glue::WebStringToString(param2),
          webkit_glue::WebStringToString(param3))) {
    return;
  }

  Vector<String> v;
  v.append(webkit_glue::WebStringToString(class_name));
  v.append(webkit_glue::WebStringToString(method_name));
  v.append(webkit_glue::WebStringToString(param1));
  v.append(webkit_glue::WebStringToString(param2));
  v.append(webkit_glue::WebStringToString(param3));
  if (!loaded_) {
    pending_incoming_messages_.append(v);
    return;
  }
  ExecuteScript(v);
}

void WebDevToolsFrontendImpl::dispatchMessageFromAgent(
    const WebKit::WebDevToolsMessageData& data) {
  // Stub.
}

void WebDevToolsFrontendImpl::AddResourceSourceToFrame(int resource_id,
                                                     String mime_type,
                                                     Node* frame) {
  if (tools_agent_native_delegate_impl_->WaitingForResponse(resource_id,
                                                            frame)) {
    return;
  }
  tools_agent_obj_->GetResourceContent(resource_id, resource_id);
  tools_agent_native_delegate_impl_->RequestSent(resource_id, mime_type, frame);
}

void WebDevToolsFrontendImpl::ExecuteScript(const Vector<String>& v) {
  WebFrameImpl* frame = web_view_impl_->mainFrameImpl();
  v8::HandleScope scope;
  v8::Handle<v8::Context> frame_context = V8Proxy::context(frame->frame());
  v8::Context::Scope context_scope(frame_context);
  v8::Handle<v8::Value> dispatch_function =
      frame_context->Global()->Get(v8::String::New("devtools$$dispatch"));
  ASSERT(dispatch_function->IsFunction());
  v8::Handle<v8::Function> function = v8::Handle<v8::Function>::Cast(dispatch_function);
  v8::Handle<v8::Value> args[] = {
    ToV8String(v.at(0)),
    ToV8String(v.at(1)),
    ToV8String(v.at(2)),
    ToV8String(v.at(3)),
    ToV8String(v.at(4)),
  };
  function->Call(frame_context->Global(), 5, args);
}

void WebDevToolsFrontendImpl::DispatchOnWebInspector(
    const String& methodName, const String& param) {
  WebFrameImpl* frame = web_view_impl_->mainFrameImpl();
  v8::HandleScope scope;
  v8::Handle<v8::Context> frame_context = V8Proxy::context(frame->frame());
  v8::Context::Scope context_scope(frame_context);

  v8::Handle<v8::Value> web_inspector =
      frame_context->Global()->Get(v8::String::New("WebInspector"));
  ASSERT(web_inspector->IsObject());
  v8::Handle<v8::Object> web_inspector_obj =
      v8::Handle<v8::Object>::Cast(web_inspector);

  v8::Handle<v8::Value> method =
      web_inspector_obj->Get(ToV8String(methodName));
  ASSERT(method->IsFunction());
  v8::Handle<v8::Function> method_func = v8::Handle<v8::Function>::Cast(method);
  v8::Handle<v8::Value> args[] = {
    ToV8String(param)
  };
  method_func->Call(frame_context->Global(), 1, args);
}


void WebDevToolsFrontendImpl::SendRpcMessage(const String& class_name,
                                           const String& method_name,
                                           const String& param1,
                                           const String& param2,
                                           const String& param3) {
  client_->sendMessageToAgent(
      webkit_glue::StringToWebString(class_name),
      webkit_glue::StringToWebString(method_name),
      webkit_glue::StringToWebString(param1),
      webkit_glue::StringToWebString(param2),
      webkit_glue::StringToWebString(param3));
}

void WebDevToolsFrontendImpl::ContextMenuItemSelected(
    ContextMenuItem* item) {
  int item_number = item->action() - ContextMenuItemBaseCustomTag;
  DispatchOnWebInspector("contextMenuItemSelected",
                         String::number(item_number));
}

void WebDevToolsFrontendImpl::ContextMenuCleared() {
  DispatchOnWebInspector("contextMenuCleared", "");
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsReset(
    const v8::Arguments& args) {
  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());
  WebFrameImpl* frame = frontend->web_view_impl_->mainFrameImpl();
  frontend->tools_agent_native_delegate_impl_.set(
      new ToolsAgentNativeDelegateImpl(frame));
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsAddSourceToFrame(
    const v8::Arguments& args) {
  if (args.Length() < 2) {
    return v8::Undefined();
  }

  v8::TryCatch exception_catcher;

  String mime_type = WebCore::toWebCoreStringWithNullCheck(args[0]);
  if (mime_type.isEmpty() || exception_catcher.HasCaught()) {
    return v8::Undefined();
  }
  String source_string = WebCore::toWebCoreStringWithNullCheck(args[1]);
  if (source_string.isEmpty() || exception_catcher.HasCaught()) {
    return v8::Undefined();
  }
  v8::Handle<v8::Object> wrapper = v8::Handle<v8::Object>::Cast(args[2]);
  Node* node = V8DOMWrapper::convertDOMWrapperToNode<Node>(wrapper);
  if (!node || !node->attached()) {
    return v8::Undefined();
  }

  Page* page = V8Proxy::retrieveFrameForEnteredContext()->page();
  InspectorController* inspectorController = page->inspectorController();
  return WebCore::v8Boolean(inspectorController->inspectorFrontendHost()->
      addSourceToFrame(mime_type, source_string, node));
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsAddResourceSourceToFrame(
    const v8::Arguments& args) {
  int resource_id = static_cast<int>(args[0]->NumberValue());
  String mime_type = WebCore::toWebCoreStringWithNullCheck(args[1]);
  if (mime_type.isEmpty()) {
    return v8::Undefined();
  }
  v8::Handle<v8::Object> wrapper = v8::Handle<v8::Object>::Cast(args[2]);
  Node* node = V8DOMWrapper::convertDOMWrapperToNode<Node>(wrapper);
  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());
  frontend->AddResourceSourceToFrame(resource_id, mime_type, node);
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsLoaded(
    const v8::Arguments& args) {
  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());
  frontend->loaded_ = true;

  // Grant the devtools page the ability to have source view iframes.
  Page* page = V8Proxy::retrieveFrameForEnteredContext()->page();
  SecurityOrigin* origin = page->mainFrame()->domWindow()->securityOrigin();
  origin->grantUniversalAccess();

  for (Vector<Vector<String> >::iterator it =
           frontend->pending_incoming_messages_.begin();
       it != frontend->pending_incoming_messages_.end();
       ++it) {
    frontend->ExecuteScript(*it);
  }
  frontend->pending_incoming_messages_.clear();
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsPlatform(
    const v8::Arguments& args) {
#if defined OS_MACOSX
  return v8String("mac-leopard");
#elif defined OS_LINUX
  return v8String("linux");
#else
  return v8String("windows");
#endif
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsPort(
    const v8::Arguments& args) {
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsActivateWindow(
    const v8::Arguments& args) {
  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());
  frontend->client_->activateWindow();
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsCloseWindow(
    const v8::Arguments& args) {
  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());
  frontend->client_->closeWindow();
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsDockWindow(
    const v8::Arguments& args) {
  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());
  frontend->client_->dockWindow();
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsUndockWindow(
    const v8::Arguments& args) {
  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());
  frontend->client_->undockWindow();
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsLocalizedStringsURL(
    const v8::Arguments& args) {
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsHiddenPanels(
    const v8::Arguments& args) {
  return v8String("");
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsDebuggerCommand(
    const v8::Arguments& args) {
  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());
  String command = WebCore::toWebCoreStringWithNullCheck(args[0]);
  WebString std_command = webkit_glue::StringToWebString(command);
  frontend->client_->sendDebuggerCommandToAgent(std_command);
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsSetting(
    const v8::Arguments& args) {
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsSetSetting(
    const v8::Arguments& args) {
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsDebuggerPauseScript(
    const v8::Arguments& args) {
  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());
  frontend->client_->sendDebuggerPauseScript();
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsWindowUnloading(
    const v8::Arguments& args) {
  //TODO(pfeldman): Implement this.
  return v8::Undefined();
}

// static
v8::Handle<v8::Value> WebDevToolsFrontendImpl::JsShowContextMenu(
    const v8::Arguments& args) {

  if (args.Length() < 2)
    return v8::Undefined();

  v8::Local<v8::Object> event_wrapper = v8::Local<v8::Object>::Cast(args[0]);
  if (V8DOMWrapper::domWrapperType(event_wrapper) != V8ClassIndex::EVENT)
    return v8::Undefined();

  Event* event = V8DOMWrapper::convertDOMWrapperToNative<Event>(event_wrapper);
  if (!args[1]->IsArray())
    return v8::Undefined();

  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(args[1]);
  Vector<ContextMenuItem> items;

  for (size_t i = 0; i < array->Length(); ++i) {
    v8::Local<v8::Object> item = v8::Local<v8::Object>::Cast(
        array->Get(v8::Integer::New(i)));
    v8::Local<v8::Value> label = item->Get(v8::String::New("label"));
    v8::Local<v8::Value> id = item->Get(v8::String::New("id"));
    if (label->IsUndefined() || id->IsUndefined()) {
      items.append(ContextMenuItem(SeparatorType,
                                   ContextMenuItemTagNoAction,
                                   String()));
    } else {
      ContextMenuAction typedId = static_cast<ContextMenuAction>(
          ContextMenuItemBaseCustomTag + id->ToInt32()->Value());
      items.append(ContextMenuItem(ActionType,
                                   typedId,
                                   toWebCoreStringWithNullCheck(label)));
    }
  }

  WebDevToolsFrontendImpl* frontend = static_cast<WebDevToolsFrontendImpl*>(
      v8::External::Cast(*args.Data())->Value());

  ContextMenuController* menu_controller = frontend->web_view_impl_->page()->
      contextMenuController();
  menu_controller->showContextMenu(event,
                                   items,
                                   frontend->menu_selection_handler_);
  return v8::Undefined();
}
