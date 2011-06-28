// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VIEWS_LAYER_HELPER_H_
#define VIEWS_LAYER_HELPER_H_
#pragma once

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "ui/gfx/rect.h"

namespace ui {
class Layer;
class Transform;
}

namespace views {

class LayerPropertySetter;

// This is a views-internal API and should not be used externally. View uses
// this class to manage fields related to accelerated painting.
namespace internal {

class LayerHelper {
 public:
  LayerHelper();
  ~LayerHelper();

  void SetTransform(const ui::Transform& transform);

  // Only returns non-null if a non-identity transform has been set.
  const ui::Transform* transform() const { return transform_.get(); }

  void SetLayer(ui::Layer* layer);
  ui::Layer* layer() { return layer_.get(); }

  // Rectangle that needs to be painted.
  void set_clip_rect(const gfx::Rect& rect) {
    clip_rect_ = rect;
  }
  const gfx::Rect& clip_rect() const { return clip_rect_; }

  // If true, the layer's bitmap is out of date and needs to be updated.
  void set_bitmap_needs_updating(bool value) {
    bitmap_needs_updating_ = value;
  }
  bool bitmap_needs_updating() const {
    return bitmap_needs_updating_;
  }

  // If true the layer was explicitly turned on.
  void set_paint_to_layer(bool value) { paint_to_layer_ = value; }
  bool paint_to_layer() const { return paint_to_layer_; }

  void SetPropertySetter(LayerPropertySetter* setter);
  LayerPropertySetter* property_setter() {
    return property_setter_.get();
  }

  // If true the LayerPropertySetter was explicitly set.
  void set_property_setter_explicitly_set(bool value) {
    property_setter_explicitly_set_ = value;
  }
  bool property_setter_explicitly_set() {
    return property_setter_explicitly_set_;
  }

  // See View::SetExternalTexture for details.
  void set_layer_updated_externally(bool value) {
    layer_updated_externally_ = value;
  }
  bool layer_updated_externally() const { return layer_updated_externally_; }

  // If true the complete bounds of the view needs to be painted.
  void set_needs_paint_all(bool value) { needs_paint_all_ = value; }
  bool needs_paint_all() const { return needs_paint_all_; }

  // Returns true if the layer needs to be used.
  bool ShouldPaintToLayer() const;

 private:
  // The transformation matrix (rotation, translate, scale). If non-null the
  // transform is not the identity transform.
  scoped_ptr<ui::Transform> transform_;

  scoped_ptr<ui::Layer> layer_;

  scoped_ptr<LayerPropertySetter> property_setter_;

  // Used during painting. If not empty and View::Paint() is invoked, the canvas
  // is created with the specified size.
  // TODO(sky): this should be passed into paint.
  gfx::Rect clip_rect_;

  // Is the layers bitmap out of date?
  bool bitmap_needs_updating_;

  // If true the bitmap is always up to date.
  bool layer_updated_externally_;

  // Should the View paint to a layer?
  bool paint_to_layer_;

  bool property_setter_explicitly_set_;

  bool needs_paint_all_;

  DISALLOW_COPY_AND_ASSIGN(LayerHelper);
};

}  // namespace internal

}  // namespace views

#endif  // VIEWS_LAYER_HELPER_H_
