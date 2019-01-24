swaybg(1)

# NAME

swaybg - Screen background for Wayland

# SYNOPSIS

_swaybg_ [options...]

Displays a background in your Wayland session.

# OPTIONS

*-h, --help*
	Show help message and quit.

*-o, --output* <output>
	Configure a particular output. Options following this will configure this
	output until another output is given. Use the wildcard _\*_ to configure a
	fallback, or specify options before the first output option.

*-c, --color* <rrggbb[aa]>
	Sets the background color to the given color.

*-i, --image* <path>
	Sets the background image to the given path. swaybg will quit if an image
	cannot be loaded.

*-m, --mode* center|fill|fit|solid_color|stretch|tile
	Sets the scaling mode for images, or, if _solid\_color_ is given, disables
	using a background image.

# SIGNALS

Receiving SIGUSR1 will cause swaybg to reload all images.

# SEE ALSO

sway(1).

# AUTHORS

Maintained by Drew DeVault <sir@cmpwn.com>, who is assisted by other open
source contributors. For more information about swaylock development, see
https://github.com/swaywm/sway.
