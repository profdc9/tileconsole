# The link-time takeover of the C library's file functions (see tde_libc.c), for programs that contain the editor.
TDE_WRAP = -Wl,--wrap=fopen,--wrap=stat,--wrap=chmod,--wrap=access,--wrap=utime,--wrap=unlink,--wrap=remove,--wrap=rmdir,--wrap=rename,--wrap=mkdir,--wrap=getcwd,--wrap=chdir,--wrap=realpath,--wrap=opendir,--wrap=readdir,--wrap=closedir
