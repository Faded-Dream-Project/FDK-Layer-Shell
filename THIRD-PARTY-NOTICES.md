# Third-Party Notices

This file lists third-party components incorporated into
FDK-Layer-Shell's source tree, with their licenses and provenance.
The package's own license is LICENSE (FDK Proprietary v1.0); the
policy it follows mirrors FDK's docs/licensing-policy.md.

---

## wlr-layer-shell-unstable-v1.xml

- Vendored at: generated/wlr-layer-shell-unstable-v1.xml
- Source: https://gitlab.freedesktop.org/wlroots/wlr-protocols
  (unstable/wlr-layer-shell-unstable-v1.xml)
- Author: Drew DeVault (© 2017) and the wlroots protocol contributors
- Used for: the layer-surface role this package speaks (the XML is
  the protocol's source of truth; the committed bindings under
  generated/ are wayland-scanner 1.23.1 output of exactly this file)

License (MIT permission grant from the XML's copyright block):

------------------------------------------------------------------------------
Copyright © 2017 Drew DeVault

Permission to use, copy, modify, distribute, and sell this
software and its documentation for any purpose is hereby granted
without fee, provided that the above copyright notice appear in
all copies and that both that copyright notice and this permission
notice appear in supporting documentation, and that the name of
the copyright holders not be used in advertising or publicity
pertaining to distribution of the software without specific,
written prior permission.  The copyright holders make no
representations about the suitability of this software for any
purpose.  It is provided "as is" without express or implied
warranty.

THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
------------------------------------------------------------------------------

## xdg-shell.xml

- Vendored at: generated/xdg-shell.xml
- Source: https://gitlab.freedesktop.org/wayland/wayland-protocols
  (stable/xdg-shell/xdg-shell.xml)
- Author: the Wayland protocol contributors (© 2008-2012 Collabora
  Ltd. / © 2012-2017 Red Hat Inc. and others — see the XML's
  copyright block)
- Used for: the xdg_popup objects layer surfaces adopt through
  `zwlr_layer_surface_v1.get_popup` (the package compiles the
  protocol's client bindings so the role adoption request can name
  the xdg_popup interface; FDK itself drives the popup machinery)

License (MIT permission grant, same shape as above — the XML's own
copyright block governs; reproduced in the file header):

------------------------------------------------------------------------------
Copyright © 2008-2012 Kristian Høgsberg
Copyright © 2012-2017 Collabora Ltd.
Copyright © 2012-2017 Red Hat Inc.

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------

## Generated bindings

generated/wlr-layer-shell-unstable-v1-client-protocol.h,
generated/wlr-layer-shell-unstable-v1-server-protocol.h,
generated/wlr-layer-shell-unstable-v1-protocol.c,
generated/xdg-shell-client-protocol.h,
generated/xdg-shell-server-protocol.h and
generated/xdg-shell-protocol.c are wayland-scanner 1.23.1 output of
the two vendored XMLs above (committed, the same posture FDK's own
Wayland backend takes for its protocols) and carry the same
licenses. Regenerate with:

    wayland-scanner client-header <xml> <out.h>
    wayland-scanner server-header <xml> <out.h>
    wayland-scanner private-code  <xml> <out.c>
