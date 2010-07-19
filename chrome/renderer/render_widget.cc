// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/render_widget.h"

#include "app/surface/transport_dib.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/message_loop.h"
#include "base/scoped_ptr.h"
#include "build/build_config.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/render_messages.h"
#include "chrome/renderer/render_process.h"
#include "chrome/renderer/render_thread.h"
#include "gfx/point.h"
#include "gfx/size.h"
#include "skia/ext/platform_canvas.h"
#include "third_party/skia/include/core/SkShader.h"
#include "third_party/WebKit/WebKit/chromium/public/WebCursorInfo.h"
#include "third_party/WebKit/WebKit/chromium/public/WebPopupMenu.h"
#include "third_party/WebKit/WebKit/chromium/public/WebPopupMenuInfo.h"
#include "third_party/WebKit/WebKit/chromium/public/WebRect.h"
#include "third_party/WebKit/WebKit/chromium/public/WebScreenInfo.h"
#include "third_party/WebKit/WebKit/chromium/public/WebSize.h"
#include "webkit/glue/webkit_glue.h"

#if defined(OS_POSIX)
#include "ipc/ipc_channel_posix.h"
#include "third_party/skia/include/core/SkPixelRef.h"
#include "third_party/skia/include/core/SkMallocPixelRef.h"
#endif  // defined(OS_POSIX)

#include "third_party/WebKit/WebKit/chromium/public/WebWidget.h"

using WebKit::WebCompositionUnderline;
using WebKit::WebCursorInfo;
using WebKit::WebInputEvent;
using WebKit::WebNavigationPolicy;
using WebKit::WebPopupMenu;
using WebKit::WebPopupMenuInfo;
using WebKit::WebRect;
using WebKit::WebScreenInfo;
using WebKit::WebSize;
using WebKit::WebTextDirection;
using WebKit::WebTextInputType;
using WebKit::WebVector;

RenderWidget::RenderWidget(RenderThreadBase* render_thread,
                           WebKit::WebPopupType popup_type)
    : routing_id_(MSG_ROUTING_NONE),
      webwidget_(NULL),
      opener_id_(MSG_ROUTING_NONE),
      render_thread_(render_thread),
      host_window_(0),
      current_paint_buf_(NULL),
      next_paint_flags_(0),
      update_reply_pending_(false),
      did_show_(false),
      is_hidden_(false),
      needs_repainting_on_restore_(false),
      has_focus_(false),
      handling_input_event_(false),
      closing_(false),
      input_method_is_active_(false),
      text_input_type_(WebKit::WebTextInputTypeNone),
      popup_type_(popup_type),
      pending_window_rect_count_(0),
      suppress_next_char_events_(false),
      is_gpu_rendering_active_(false) {
  RenderProcess::current()->AddRefProcess();
  DCHECK(render_thread_);
}

RenderWidget::~RenderWidget() {
  DCHECK(!webwidget_) << "Leaking our WebWidget!";
  if (current_paint_buf_) {
    RenderProcess::current()->ReleaseTransportDIB(current_paint_buf_);
    current_paint_buf_ = NULL;
  }
  RenderProcess::current()->ReleaseProcess();
}

/*static*/
RenderWidget* RenderWidget::Create(int32 opener_id,
                                   RenderThreadBase* render_thread,
                                   WebKit::WebPopupType popup_type) {
  DCHECK(opener_id != MSG_ROUTING_NONE);
  scoped_refptr<RenderWidget> widget = new RenderWidget(render_thread,
                                                        popup_type);
  widget->Init(opener_id);  // adds reference
  return widget;
}

void RenderWidget::ConfigureAsExternalPopupMenu(const WebPopupMenuInfo& info) {
  popup_params_.reset(new ViewHostMsg_ShowPopup_Params);
  popup_params_->item_height = info.itemHeight;
  popup_params_->item_font_size = info.itemFontSize;
  popup_params_->selected_item = info.selectedIndex;
  for (size_t i = 0; i < info.items.size(); ++i)
    popup_params_->popup_items.push_back(WebMenuItem(info.items[i]));
  popup_params_->right_aligned = info.rightAligned;
}

void RenderWidget::Init(int32 opener_id) {
  DCHECK(!webwidget_);

  if (opener_id != MSG_ROUTING_NONE)
    opener_id_ = opener_id;

  webwidget_ = WebPopupMenu::create(this);

  bool result = render_thread_->Send(
      new ViewHostMsg_CreateWidget(opener_id, popup_type_, &routing_id_));
  if (result) {
    render_thread_->AddRoute(routing_id_, this);
    // Take a reference on behalf of the RenderThread.  This will be balanced
    // when we receive ViewMsg_Close.
    AddRef();
  } else {
    DCHECK(false);
  }
}

// This is used to complete pending inits and non-pending inits. For non-
// pending cases, the parent will be the same as the current parent. This
// indicates we do not need to reparent or anything.
void RenderWidget::CompleteInit(gfx::NativeViewId parent_hwnd) {
  DCHECK(routing_id_ != MSG_ROUTING_NONE);

  host_window_ = parent_hwnd;

  Send(new ViewHostMsg_RenderViewReady(routing_id_));
}

IPC_DEFINE_MESSAGE_MAP(RenderWidget)
  IPC_MESSAGE_HANDLER(ViewMsg_Close, OnClose)
  IPC_MESSAGE_HANDLER(ViewMsg_CreatingNew_ACK, OnCreatingNewAck)
  IPC_MESSAGE_HANDLER(ViewMsg_Resize, OnResize)
  IPC_MESSAGE_HANDLER(ViewMsg_WasHidden, OnWasHidden)
  IPC_MESSAGE_HANDLER(ViewMsg_WasRestored, OnWasRestored)
  IPC_MESSAGE_HANDLER(ViewMsg_UpdateRect_ACK, OnUpdateRectAck)
  IPC_MESSAGE_HANDLER(ViewMsg_CreateVideo_ACK, OnCreateVideoAck)
  IPC_MESSAGE_HANDLER(ViewMsg_UpdateVideo_ACK, OnUpdateVideoAck)
  IPC_MESSAGE_HANDLER(ViewMsg_HandleInputEvent, OnHandleInputEvent)
  IPC_MESSAGE_HANDLER(ViewMsg_MouseCaptureLost, OnMouseCaptureLost)
  IPC_MESSAGE_HANDLER(ViewMsg_SetFocus, OnSetFocus)
  IPC_MESSAGE_HANDLER(ViewMsg_SetInputMethodActive, OnSetInputMethodActive)
  IPC_MESSAGE_HANDLER(ViewMsg_ImeSetComposition, OnImeSetComposition)
  IPC_MESSAGE_HANDLER(ViewMsg_ImeConfirmComposition, OnImeConfirmComposition)
  IPC_MESSAGE_HANDLER(ViewMsg_PaintAtSize, OnMsgPaintAtSize)
  IPC_MESSAGE_HANDLER(ViewMsg_Repaint, OnMsgRepaint)
  IPC_MESSAGE_HANDLER(ViewMsg_SetTextDirection, OnSetTextDirection)
  IPC_MESSAGE_HANDLER(ViewMsg_Move_ACK, OnRequestMoveAck)
  IPC_MESSAGE_UNHANDLED_ERROR()
IPC_END_MESSAGE_MAP()

bool RenderWidget::Send(IPC::Message* message) {
  // Don't send any messages after the browser has told us to close.
  if (closing_) {
    delete message;
    return false;
  }

  // If given a messsage without a routing ID, then assign our routing ID.
  if (message->routing_id() == MSG_ROUTING_NONE)
    message->set_routing_id(routing_id_);

  return render_thread_->Send(message);
}

// Got a response from the browser after the renderer decided to create a new
// view.
void RenderWidget::OnCreatingNewAck(gfx::NativeViewId parent) {
  DCHECK(routing_id_ != MSG_ROUTING_NONE);

  CompleteInit(parent);
}

void RenderWidget::OnClose() {
  if (closing_)
    return;
  closing_ = true;

  // Browser correspondence is no longer needed at this point.
  if (routing_id_ != MSG_ROUTING_NONE) {
    render_thread_->RemoveRoute(routing_id_);
    SetHidden(false);
  }

  // If there is a Send call on the stack, then it could be dangerous to close
  // now.  Post a task that only gets invoked when there are no nested message
  // loops.
  MessageLoop::current()->PostNonNestableTask(FROM_HERE,
      NewRunnableMethod(this, &RenderWidget::Close));

  // Balances the AddRef taken when we called AddRoute.
  Release();
}

void RenderWidget::OnResize(const gfx::Size& new_size,
                            const gfx::Rect& resizer_rect) {
  // During shutdown we can just ignore this message.
  if (!webwidget_)
    return;

  // Remember the rect where the resize corner will be drawn.
  resizer_rect_ = resizer_rect;

  // TODO(darin): We should not need to reset this here.
  SetHidden(false);
  needs_repainting_on_restore_ = false;

  // We shouldn't be asked to resize to our current size.
  DCHECK(size_ != new_size);
  size_ = new_size;

  // We should not be sent a Resize message if we have not ACK'd the previous
  DCHECK(!next_paint_is_resize_ack());

  paint_aggregator_.ClearPendingUpdate();

  // When resizing, we want to wait to paint before ACK'ing the resize.  This
  // ensures that we only resize as fast as we can paint.  We only need to send
  // an ACK if we are resized to a non-empty rect.
  webwidget_->resize(new_size);
  if (!new_size.IsEmpty()) {
    // Resize should have caused an invalidation of the entire view.
    DCHECK(paint_aggregator_.HasPendingUpdate());

    // We will send the Resize_ACK flag once we paint again.
    set_next_paint_is_resize_ack();
  }
}

void RenderWidget::OnWasHidden() {
  // Go into a mode where we stop generating paint and scrolling events.
  SetHidden(true);
}

void RenderWidget::OnWasRestored(bool needs_repainting) {
  // During shutdown we can just ignore this message.
  if (!webwidget_)
    return;

  // See OnWasHidden
  SetHidden(false);

  if (!needs_repainting && !needs_repainting_on_restore_)
    return;
  needs_repainting_on_restore_ = false;

  // Tag the next paint as a restore ack, which is picked up by
  // DoDeferredUpdate when it sends out the next PaintRect message.
  set_next_paint_is_restore_ack();

  // Generate a full repaint.
  didInvalidateRect(gfx::Rect(size_.width(), size_.height()));
}

void RenderWidget::OnRequestMoveAck() {
  DCHECK(pending_window_rect_count_);
  pending_window_rect_count_--;
}

void RenderWidget::OnUpdateRectAck() {
  DCHECK(update_reply_pending());
  update_reply_pending_ = false;

  // If we sent an UpdateRect message with a zero-sized bitmap, then we should
  // have no current paint buffer.
  if (current_paint_buf_) {
    RenderProcess::current()->ReleaseTransportDIB(current_paint_buf_);
    current_paint_buf_ = NULL;
  }

  // Notify subclasses.
  DidFlushPaint();

  // Continue painting if necessary...
  CallDoDeferredUpdate();
}

void RenderWidget::OnCreateVideoAck(int32 video_id) {
  // TODO(scherkus): handle CreateVideo_ACK with a message filter.
}

void RenderWidget::OnUpdateVideoAck(int32 video_id) {
  // TODO(scherkus): handle UpdateVideo_ACK with a message filter.
}

void RenderWidget::OnHandleInputEvent(const IPC::Message& message) {
  void* iter = NULL;

  const char* data;
  int data_length;
  handling_input_event_ = true;
  if (!message.ReadData(&iter, &data, &data_length)) {
    handling_input_event_ = false;
    return;
  }

  const WebInputEvent* input_event =
      reinterpret_cast<const WebInputEvent*>(data);

  bool is_keyboard_shortcut = false;
  // is_keyboard_shortcut flag is only available for RawKeyDown events.
  if (input_event->type == WebInputEvent::RawKeyDown)
    message.ReadBool(&iter, &is_keyboard_shortcut);

  bool processed = false;
  if (input_event->type != WebInputEvent::Char || !suppress_next_char_events_) {
    suppress_next_char_events_ = false;
    if (webwidget_)
      processed = webwidget_->handleInputEvent(*input_event);
  }

  // If this RawKeyDown event corresponds to a browser keyboard shortcut and
  // it's not processed by webkit, then we need to suppress the upcoming Char
  // events.
  if (!processed && is_keyboard_shortcut)
    suppress_next_char_events_ = true;

  IPC::Message* response = new ViewHostMsg_HandleInputEvent_ACK(routing_id_);
  response->WriteInt(input_event->type);
  response->WriteBool(processed);

  if ((input_event->type == WebInputEvent::MouseMove ||
       input_event->type == WebInputEvent::MouseWheel) &&
      paint_aggregator_.HasPendingUpdate()) {
    // We want to rate limit the input events in this case, so we'll wait for
    // painting to finish before ACKing this message.
    if (pending_input_event_ack_.get()) {
      // As two different kinds of events could cause us to postpone an ack
      // we send it now, if we have one pending. The Browser should never
      // send us the same kind of event we are delaying the ack for.
      Send(pending_input_event_ack_.release());
    }
    pending_input_event_ack_.reset(response);
  } else {
    Send(response);
  }

  handling_input_event_ = false;

  if (WebInputEvent::isKeyboardEventType(input_event->type))
    DidHandleKeyEvent();
}

void RenderWidget::OnMouseCaptureLost() {
  if (webwidget_)
    webwidget_->mouseCaptureLost();
}

void RenderWidget::OnSetFocus(bool enable) {
  has_focus_ = enable;
  if (webwidget_)
    webwidget_->setFocus(enable);
}

void RenderWidget::ClearFocus() {
  // We may have got the focus from the browser before this gets processed, in
  // which case we do not want to unfocus ourself.
  if (!has_focus_ && webwidget_)
    webwidget_->setFocus(false);
}

void RenderWidget::PaintRect(const gfx::Rect& rect,
                             const gfx::Point& canvas_origin,
                             skia::PlatformCanvas* canvas) {
  canvas->save();

  // Bring the canvas into the coordinate system of the paint rect.
  canvas->translate(static_cast<SkScalar>(-canvas_origin.x()),
                    static_cast<SkScalar>(-canvas_origin.y()));

  // If there is a custom background, tile it.
  if (!background_.empty()) {
    SkPaint paint;
    SkShader* shader = SkShader::CreateBitmapShader(background_,
                                                    SkShader::kRepeat_TileMode,
                                                    SkShader::kRepeat_TileMode);
    paint.setShader(shader)->unref();
    paint.setXfermodeMode(SkXfermode::kSrcOver_Mode);
    canvas->drawPaint(paint);
  }

  webwidget_->paint(webkit_glue::ToWebCanvas(canvas), rect);

  PaintDebugBorder(rect, canvas);

  // Flush to underlying bitmap.  TODO(darin): is this needed?
  canvas->getTopPlatformDevice().accessBitmap(false);

  canvas->restore();
}

void RenderWidget::PaintDebugBorder(const gfx::Rect& rect,
                                    skia::PlatformCanvas* canvas) {
  static bool kPaintBorder =
      CommandLine::ForCurrentProcess()->HasSwitch(switches::kShowPaintRects);
  if (!kPaintBorder)
    return;

  // Cycle through these colors to help distinguish new paint rects.
  const SkColor colors[] = {
    SkColorSetARGB(0x3F, 0xFF, 0, 0),
    SkColorSetARGB(0x3F, 0xFF, 0, 0xFF),
    SkColorSetARGB(0x3F, 0, 0, 0xFF),
  };
  static int color_selector = 0;

  SkPaint paint;
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setColor(colors[color_selector++ % arraysize(colors)]);
  paint.setStrokeWidth(1);

  SkIRect irect;
  irect.set(rect.x(), rect.y(), rect.right() - 1, rect.bottom() - 1);
  canvas->drawIRect(irect, paint);
}

void RenderWidget::CallDoDeferredUpdate() {
  DoDeferredUpdate();

  if (pending_input_event_ack_.get())
    Send(pending_input_event_ack_.release());
}

void RenderWidget::DoDeferredUpdate() {
  if (!webwidget_ || !paint_aggregator_.HasPendingUpdate() ||
      update_reply_pending())
    return;

  // Suppress updating when we are hidden.
  if (is_hidden_ || size_.IsEmpty()) {
    paint_aggregator_.ClearPendingUpdate();
    needs_repainting_on_restore_ = true;
    return;
  }

  // If we are using accelerated compositing then all the drawing
  // to the associated window happens directly from the gpu process and the
  // browser process shouldn't do any drawing.
  // TODO(vangelis): Currently the accelerated compositing path relies on
  // invalidating parts of the page so that we get a request to redraw.
  // This needs to change to a model where the compositor updates the
  // contents of the page independently and the browser process gets no
  // longer involved.
  if (webwidget_->isAcceleratedCompositingActive() !=
      is_gpu_rendering_active_) {
    is_gpu_rendering_active_ = webwidget_->isAcceleratedCompositingActive();
    Send(new ViewHostMsg_GpuRenderingActivated(
        routing_id_, is_gpu_rendering_active_));
  }

  // Layout may generate more invalidation.
  webwidget_->layout();

  // OK, save the pending update to a local since painting may cause more
  // invalidation.  Some WebCore rendering objects only layout when painted.
  PaintAggregator::PendingUpdate update = paint_aggregator_.GetPendingUpdate();
  paint_aggregator_.ClearPendingUpdate();

  gfx::Rect scroll_damage = update.GetScrollDamage();
  gfx::Rect bounds = update.GetPaintBounds().Union(scroll_damage);

  // Compute a buffer for painting and cache it.
  scoped_ptr<skia::PlatformCanvas> canvas(
      RenderProcess::current()->GetDrawingCanvas(&current_paint_buf_, bounds));
  if (!canvas.get()) {
    NOTREACHED();
    return;
  }

  // We may get back a smaller canvas than we asked for.
  // TODO(darin): This seems like it could cause painting problems!
  DCHECK_EQ(bounds.width(), canvas->getDevice()->width());
  DCHECK_EQ(bounds.height(), canvas->getDevice()->height());
  bounds.set_width(canvas->getDevice()->width());
  bounds.set_height(canvas->getDevice()->height());

  HISTOGRAM_COUNTS_100("MPArch.RW_PaintRectCount", update.paint_rects.size());

  // TODO(darin): Re-enable painting multiple damage rects once the
  // page-cycler regressions are resolved.  See bug 29589.
  if (update.scroll_rect.IsEmpty()) {
    update.paint_rects.clear();
    update.paint_rects.push_back(bounds);
  }

  // The scroll damage is just another rectangle to paint and copy.
  std::vector<gfx::Rect> copy_rects;
  copy_rects.swap(update.paint_rects);
  if (!scroll_damage.IsEmpty())
    copy_rects.push_back(scroll_damage);

  for (size_t i = 0; i < copy_rects.size(); ++i)
    PaintRect(copy_rects[i], bounds.origin(), canvas.get());

  ViewHostMsg_UpdateRect_Params params;
  params.bitmap = current_paint_buf_->id();
  params.bitmap_rect = bounds;
  params.dx = update.scroll_delta.x();
  params.dy = update.scroll_delta.y();
  if (is_gpu_rendering_active_) {
    // If painting is done via the gpu process then we clear out all damage
    // rects to save the browser process from doing unecessary work.
    params.scroll_rect = gfx::Rect();
    params.copy_rects.clear();
  } else {
    params.scroll_rect = update.scroll_rect;
    params.copy_rects.swap(copy_rects);  // TODO(darin): clip to bounds?
  }
  params.view_size = size_;
  params.plugin_window_moves.swap(plugin_window_moves_);
  params.flags = next_paint_flags_;

  update_reply_pending_ = true;
  Send(new ViewHostMsg_UpdateRect(routing_id_, params));
  next_paint_flags_ = 0;

  UpdateInputMethod();

  // Let derived classes know we've painted.
  DidInitiatePaint();
}

///////////////////////////////////////////////////////////////////////////////
// WebWidgetDelegate

void RenderWidget::didInvalidateRect(const WebRect& rect) {
  // We only want one pending DoDeferredUpdate call at any time...
  bool update_pending = paint_aggregator_.HasPendingUpdate();

  // The invalidated rect might be outside the bounds of the view.
  gfx::Rect view_rect(size_);
  gfx::Rect damaged_rect = view_rect.Intersect(rect);
  if (damaged_rect.IsEmpty())
    return;

  paint_aggregator_.InvalidateRect(damaged_rect);

  // We may not need to schedule another call to DoDeferredUpdate.
  if (update_pending)
    return;
  if (!paint_aggregator_.HasPendingUpdate())
    return;
  if (update_reply_pending())
    return;

  // Perform updating asynchronously.  This serves two purposes:
  // 1) Ensures that we call WebView::Paint without a bunch of other junk
  //    on the call stack.
  // 2) Allows us to collect more damage rects before painting to help coalesce
  //    the work that we will need to do.
  MessageLoop::current()->PostTask(FROM_HERE, NewRunnableMethod(
      this, &RenderWidget::CallDoDeferredUpdate));
}

void RenderWidget::didScrollRect(int dx, int dy, const WebRect& clip_rect) {
  // We only want one pending DoDeferredUpdate call at any time...
  bool update_pending = paint_aggregator_.HasPendingUpdate();

  // The scrolled rect might be outside the bounds of the view.
  gfx::Rect view_rect(size_);
  gfx::Rect damaged_rect = view_rect.Intersect(clip_rect);
  if (damaged_rect.IsEmpty())
    return;

  paint_aggregator_.ScrollRect(dx, dy, damaged_rect);

  // We may not need to schedule another call to DoDeferredUpdate.
  if (update_pending)
    return;
  if (!paint_aggregator_.HasPendingUpdate())
    return;
  if (update_reply_pending())
    return;

  // Perform updating asynchronously.  This serves two purposes:
  // 1) Ensures that we call WebView::Paint without a bunch of other junk
  //    on the call stack.
  // 2) Allows us to collect more damage rects before painting to help coalesce
  //    the work that we will need to do.
  MessageLoop::current()->PostTask(FROM_HERE, NewRunnableMethod(
      this, &RenderWidget::CallDoDeferredUpdate));
}

void RenderWidget::didChangeCursor(const WebCursorInfo& cursor_info) {
  // TODO(darin): Eliminate this temporary.
  WebCursor cursor(cursor_info);

  // Only send a SetCursor message if we need to make a change.
  if (!current_cursor_.IsEqual(cursor)) {
    current_cursor_ = cursor;
    Send(new ViewHostMsg_SetCursor(routing_id_, cursor));
  }
}

// We are supposed to get a single call to Show for a newly created RenderWidget
// that was created via RenderWidget::CreateWebView.  So, we wait until this
// point to dispatch the ShowWidget message.
//
// This method provides us with the information about how to display the newly
// created RenderWidget (i.e., as a constrained popup or as a new tab).
//
void RenderWidget::show(WebNavigationPolicy) {
  DCHECK(!did_show_) << "received extraneous Show call";
  DCHECK(routing_id_ != MSG_ROUTING_NONE);
  DCHECK(opener_id_ != MSG_ROUTING_NONE);

  if (!did_show_) {
    did_show_ = true;
    // NOTE: initial_pos_ may still have its default values at this point, but
    // that's okay.  It'll be ignored if as_popup is false, or the browser
    // process will impose a default position otherwise.
    if (popup_params_.get()) {
      popup_params_->bounds = initial_pos_;
      Send(new ViewHostMsg_ShowPopup(routing_id_, *popup_params_));
      popup_params_.reset();
    } else {
      Send(new ViewHostMsg_ShowWidget(opener_id_, routing_id_, initial_pos_));
    }
    SetPendingWindowRect(initial_pos_);
  }
}

void RenderWidget::didFocus() {
  // Prevent the widget from stealing the focus if it does not have focus
  // already.  We do this by explicitely setting the focus to false again.
  // We only let the browser focus the renderer.
  if (!has_focus_ && webwidget_) {
    MessageLoop::current()->PostTask(FROM_HERE,
        NewRunnableMethod(this, &RenderWidget::ClearFocus));
  }
}

void RenderWidget::didBlur() {
  Send(new ViewHostMsg_Blur(routing_id_));
}

void RenderWidget::DoDeferredClose() {
  Send(new ViewHostMsg_Close(routing_id_));
}

void RenderWidget::closeWidgetSoon() {
  // If a page calls window.close() twice, we'll end up here twice, but that's
  // OK.  It is safe to send multiple Close messages.

  // Ask the RenderWidgetHost to initiate close.  We could be called from deep
  // in Javascript.  If we ask the RendwerWidgetHost to close now, the window
  // could be closed before the JS finishes executing.  So instead, post a
  // message back to the message loop, which won't run until the JS is
  // complete, and then the Close message can be sent.
  MessageLoop::current()->PostTask(FROM_HERE, NewRunnableMethod(
      this, &RenderWidget::DoDeferredClose));
}

void RenderWidget::GenerateFullRepaint() {
  didInvalidateRect(gfx::Rect(size_.width(), size_.height()));
}

void RenderWidget::Close() {
  if (webwidget_) {
    webwidget_->close();
    webwidget_ = NULL;
  }
}

WebRect RenderWidget::windowRect() {
  if (pending_window_rect_count_)
    return pending_window_rect_;

  gfx::Rect rect;
  Send(new ViewHostMsg_GetWindowRect(routing_id_, host_window_, &rect));
  return rect;
}

void RenderWidget::setWindowRect(const WebRect& pos) {
  if (did_show_) {
    Send(new ViewHostMsg_RequestMove(routing_id_, pos));
    SetPendingWindowRect(pos);
  } else {
    initial_pos_ = pos;
  }
}

void RenderWidget::SetPendingWindowRect(const WebRect& rect) {
  pending_window_rect_ = rect;
  pending_window_rect_count_++;
}

WebRect RenderWidget::rootWindowRect() {
  if (pending_window_rect_count_) {
    // NOTE(mbelshe): If there is a pending_window_rect_, then getting
    // the RootWindowRect is probably going to return wrong results since the
    // browser may not have processed the Move yet.  There isn't really anything
    // good to do in this case, and it shouldn't happen - since this size is
    // only really needed for windowToScreen, which is only used for Popups.
    return pending_window_rect_;
  }

  gfx::Rect rect;
  Send(new ViewHostMsg_GetRootWindowRect(routing_id_, host_window_, &rect));
  return rect;
}

WebRect RenderWidget::windowResizerRect() {
  return resizer_rect_;
}

void RenderWidget::OnSetInputMethodActive(bool is_active) {
  // To prevent this renderer process from sending unnecessary IPC messages to
  // a browser process, we permit the renderer process to send IPC messages
  // only during the input method attached to the browser process is active.
  input_method_is_active_ = is_active;
}

void RenderWidget::OnImeSetComposition(
    const string16& text,
    const std::vector<WebCompositionUnderline>& underlines,
    int selection_start, int selection_end) {
  if (!webwidget_)
    return;
  if (!webwidget_->setComposition(
      text, WebVector<WebCompositionUnderline>(underlines),
      selection_start, selection_end)) {
    // If we failed to set the composition text, then we need to let the browser
    // process to cancel the input method's ongoing composition session, to make
    // sure we are in a consistent state.
    Send(new ViewHostMsg_ImeCancelComposition(routing_id()));
  }
}

void RenderWidget::OnImeConfirmComposition() {
  if (webwidget_)
    webwidget_->confirmComposition();
}

// This message causes the renderer to render an image of the
// desired_size, regardless of whether the tab is hidden or not.
void RenderWidget::OnMsgPaintAtSize(const TransportDIB::Handle& dib_handle,
                                    int tag,
                                    const gfx::Size& page_size,
                                    const gfx::Size& desired_size) {
  if (!webwidget_ || dib_handle == TransportDIB::DefaultHandleValue())
    return;

  if (page_size.IsEmpty() || desired_size.IsEmpty()) {
    // If one of these is empty, then we just return the dib we were
    // given, to avoid leaking it.
    Send(new ViewHostMsg_PaintAtSize_ACK(routing_id_, tag, desired_size));
    return;
  }

  // Map the given DIB ID into this process, and unmap it at the end
  // of this function.
  scoped_ptr<TransportDIB> paint_at_size_buffer(TransportDIB::Map(dib_handle));

  DCHECK(paint_at_size_buffer.get());
  if (!paint_at_size_buffer.get())
    return;

  gfx::Size canvas_size = page_size;
  float x_scale = static_cast<float>(desired_size.width()) /
                  static_cast<float>(canvas_size.width());
  float y_scale = static_cast<float>(desired_size.height()) /
                  static_cast<float>(canvas_size.height());

  gfx::Rect orig_bounds(canvas_size);
  canvas_size.set_width(static_cast<int>(canvas_size.width() * x_scale));
  canvas_size.set_height(static_cast<int>(canvas_size.height() * y_scale));
  gfx::Rect bounds(canvas_size);

  scoped_ptr<skia::PlatformCanvas> canvas(
      paint_at_size_buffer->GetPlatformCanvas(canvas_size.width(),
                                              canvas_size.height()));
  if (!canvas.get()) {
    NOTREACHED();
    return;
  }

  // Reset bounds to what we actually received, but they should be the
  // same.
  DCHECK_EQ(bounds.width(), canvas->getDevice()->width());
  DCHECK_EQ(bounds.height(), canvas->getDevice()->height());
  bounds.set_width(canvas->getDevice()->width());
  bounds.set_height(canvas->getDevice()->height());

  canvas->save();
  // Add the scale factor to the canvas, so that we'll get the desired size.
  canvas->scale(SkFloatToScalar(x_scale), SkFloatToScalar(y_scale));

  // Have to make sure we're laid out at the right size before
  // rendering.
  gfx::Size old_size = webwidget_->size();
  webwidget_->resize(page_size);
  webwidget_->layout();

  // Paint the entire thing (using original bounds, not scaled bounds).
  PaintRect(orig_bounds, orig_bounds.origin(), canvas.get());
  canvas->restore();

  // Return the widget to its previous size.
  webwidget_->resize(old_size);

  Send(new ViewHostMsg_PaintAtSize_ACK(routing_id_, tag, bounds.size()));
}

void RenderWidget::OnMsgRepaint(const gfx::Size& size_to_paint) {
  // During shutdown we can just ignore this message.
  if (!webwidget_)
    return;

  set_next_paint_is_repaint_ack();
  gfx::Rect repaint_rect(size_to_paint.width(), size_to_paint.height());
  didInvalidateRect(repaint_rect);
}

void RenderWidget::OnSetTextDirection(WebTextDirection direction) {
  if (!webwidget_)
    return;
  webwidget_->setTextDirection(direction);
}

void RenderWidget::SetHidden(bool hidden) {
  if (is_hidden_ == hidden)
    return;

  // The status has changed.  Tell the RenderThread about it.
  is_hidden_ = hidden;
  if (is_hidden_)
    render_thread_->WidgetHidden();
  else
    render_thread_->WidgetRestored();
}

void RenderWidget::SetBackground(const SkBitmap& background) {
  background_ = background;
  // Generate a full repaint.
  didInvalidateRect(gfx::Rect(size_.width(), size_.height()));
}

bool RenderWidget::next_paint_is_resize_ack() const {
  return ViewHostMsg_UpdateRect_Flags::is_resize_ack(next_paint_flags_);
}

bool RenderWidget::next_paint_is_restore_ack() const {
  return ViewHostMsg_UpdateRect_Flags::is_restore_ack(next_paint_flags_);
}

void RenderWidget::set_next_paint_is_resize_ack() {
  next_paint_flags_ |= ViewHostMsg_UpdateRect_Flags::IS_RESIZE_ACK;
}

void RenderWidget::set_next_paint_is_restore_ack() {
  next_paint_flags_ |= ViewHostMsg_UpdateRect_Flags::IS_RESTORE_ACK;
}

void RenderWidget::set_next_paint_is_repaint_ack() {
  next_paint_flags_ |= ViewHostMsg_UpdateRect_Flags::IS_REPAINT_ACK;
}

void RenderWidget::UpdateInputMethod() {
  if (!input_method_is_active_)
    return;

  WebTextInputType new_type = WebKit::WebTextInputTypeNone;
  WebRect new_caret_bounds;

  if (webwidget_) {
    new_type = webwidget_->textInputType();
    new_caret_bounds = webwidget_->caretOrSelectionBounds();
  }

  // Only sends text input type and caret bounds to the browser process if they
  // are changed.
  if (text_input_type_ != new_type || caret_bounds_ != new_caret_bounds) {
    text_input_type_ = new_type;
    caret_bounds_ = new_caret_bounds;
    Send(new ViewHostMsg_ImeUpdateTextInputState(
        routing_id(), new_type, new_caret_bounds));
  }
}

WebScreenInfo RenderWidget::screenInfo() {
  WebScreenInfo results;
  Send(new ViewHostMsg_GetScreenInfo(routing_id_, host_window_, &results));
  return results;
}

void RenderWidget::resetInputMethod() {
  if (!input_method_is_active_)
    return;

  // If the last text input type is not None, then we should finish any
  // ongoing composition regardless of the new text input type.
  if (text_input_type_ != WebKit::WebTextInputTypeNone) {
    // If a composition text exists, then we need to let the browser process
    // to cancel the input method's ongoing composition session.
    if (webwidget_->confirmComposition())
      Send(new ViewHostMsg_ImeCancelComposition(routing_id()));
  }
}

void RenderWidget::SchedulePluginMove(
    const webkit_glue::WebPluginGeometry& move) {
  size_t i = 0;
  for (; i < plugin_window_moves_.size(); ++i) {
    if (plugin_window_moves_[i].window == move.window) {
      if (move.rects_valid) {
        plugin_window_moves_[i] = move;
      } else {
        plugin_window_moves_[i].visible = move.visible;
      }
      break;
    }
  }

  if (i == plugin_window_moves_.size())
    plugin_window_moves_.push_back(move);
}

void RenderWidget::CleanupWindowInPluginMoves(gfx::PluginWindowHandle window) {
  for (WebPluginGeometryVector::iterator i = plugin_window_moves_.begin();
       i != plugin_window_moves_.end(); ++i) {
    if (i->window == window) {
      plugin_window_moves_.erase(i);
      break;
    }
  }
}
