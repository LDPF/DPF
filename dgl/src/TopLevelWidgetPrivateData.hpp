/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2021 Filipe Coelho <falktx@falktx.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose with
 * or without fee is hereby granted, provided that the above copyright notice and this
 * permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
 * TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef DGL_TOP_LEVEL_WIDGET_PRIVATE_DATA_HPP_INCLUDED
#define DGL_TOP_LEVEL_WIDGET_PRIVATE_DATA_HPP_INCLUDED

#include "../TopLevelWidget.hpp"

#include <list>

START_NAMESPACE_DGL

// -----------------------------------------------------------------------

struct TopLevelWidget::PrivateData {
    TopLevelWidget* const self;
    Widget* const selfw;
    Window& window;

    explicit PrivateData(TopLevelWidget* const s, Window& w);
    ~PrivateData();
    void display();
    bool keyboardEvent(const KeyboardEvent& ev);
    bool specialEvent(const SpecialEvent& ev);
    bool characterInputEvent(const CharacterInputEvent& ev);
    bool mouseEvent(const MouseEvent& ev);
    bool motionEvent(const MotionEvent& ev);
    bool scrollEvent(const ScrollEvent& ev);
    void fallbackOnResize();

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PrivateData)
};

// -----------------------------------------------------------------------

END_NAMESPACE_DGL

#endif // DGL_TOP_LEVEL_WIDGET_PRIVATE_DATA_HPP_INCLUDED
