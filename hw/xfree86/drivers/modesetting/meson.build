modesetting_srcs = [
    'dri2.c',
    'driver.c',
    'drmmode_display.c',
    'dumb_bo.c',
    'pageflip.c',
    'present.c',
    'vblank.c',
]

shared_module(
    'modesetting_drv',
    modesetting_srcs,
    name_prefix: '',

    include_directories: [inc, xorg_inc],
    c_args: xorg_c_args,
    dependencies: [
        common_dep,
        udev_dep,
        libdrm_dep,
    ],

    install: true,
    install_dir: join_paths(module_dir, 'drivers'),
)

# Test that we don't have any unresolved symbols from our module to Xorg.
xorg_build_root = join_paths(meson.build_root(), 'hw', 'xfree86')
symbol_test_args = []
symbol_test_args += join_paths(xorg_build_root, 'libxorgserver.so')
symbol_test_args += join_paths(xorg_build_root, 'dixmods', 'libfb.so')
symbol_test_args += join_paths(xorg_build_root, 'dixmods', 'libshadow.so')
if build_glamor
    symbol_test_args += join_paths(xorg_build_root, 'glamor_egl', 'libglamoregl.so')
endif
symbol_test_args += join_paths(xorg_build_root, 'drivers', 'modesetting', 'modesetting_drv.so')

test('modesetting symbol test',
    xorg_symbol_test,
    args: symbol_test_args,
)
