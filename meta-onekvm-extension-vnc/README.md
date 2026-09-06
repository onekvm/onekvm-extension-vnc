# OpenEmbedded layer

This layer packages the VNC extension for OneKVM. It supports the `wrynose`
release and depends on OE-Core plus the distro-owned `onekvm` layer, which
provides `onekvm-extension.bbclass`.

The integrating distro must set `ONEKVM_COMPONENT_SRCREV` to the fixed
component commit that supplies this layer. The LibVNCServer upstream revision
remains independently pinned by the recipe.
