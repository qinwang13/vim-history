/* os_msdos.c */
void mch_set_normal_colors __ARGS((void));
void mch_update_cursor __ARGS((void));
long_u mch_avail_mem __ARGS((int special));
void mch_delay __ARGS((long msec, int ignoreinput));
void mch_write __ARGS((char_u *s, int len));
int mch_inchar __ARGS((char_u *buf, int maxlen, long time));
int mch_char_avail __ARGS((void));
void mch_suspend __ARGS((void));
void mch_init __ARGS((void));
int mch_check_win __ARGS((int argc, char **argv));
int mch_input_isatty __ARGS((void));
void fname_case __ARGS((char_u *name, int len));
long mch_get_pid __ARGS((void));
int mch_FullName __ARGS((char_u *fname, char_u *buf, int len, int force));
void slash_adjust __ARGS((char_u *p));
int mch_isFullName __ARGS((char_u *fname));
void mch_early_init __ARGS((void));
void mch_exit __ARGS((int r));
void mch_settmode __ARGS((int tmode));
void mch_setmouse __ARGS((int on));
int mch_screenmode __ARGS((char_u *arg));
int mch_get_shellsize __ARGS((void));
void mch_set_shellsize __ARGS((void));
void mch_new_shellsize __ARGS((void));
void mch_check_columns __ARGS((void));
int mch_call_shell __ARGS((char_u *cmd, int options));
void mch_breakcheck __ARGS((void));
int mch_has_exp_wildcard __ARGS((char_u *p));
int mch_has_wildcard __ARGS((char_u *p));
int mch_chdir __ARGS((char *path));
char *djgpp_setlocale __ARGS((void));
int clip_mch_own_selection __ARGS((VimClipboard *cbd));
void clip_mch_lose_selection __ARGS((VimClipboard *cbd));
void clip_mch_request_selection __ARGS((VimClipboard *cbd));
void clip_mch_set_selection __ARGS((VimClipboard *cbd));
int OpenClipboard __ARGS((void));
int CloseClipboard __ARGS((void));
int EmptyClipboard __ARGS((void));
void FreeDOSMemory __ARGS((int protected_mode_selector));
char_u *GetClipboardData __ARGS((int clip_data_format));
int SetClipboardData __ARGS((int clip_data_format, char_u *clip_data, int clip_data_size, int clip_data_type));
long mch_getperm __ARGS((char_u *name));
int mch_setperm __ARGS((char_u *name, long perm));
void mch_hide __ARGS((char_u *name));
int mch_isdir __ARGS((char_u *name));
int mch_can_exe __ARGS((char_u *name));
int mch_nodetype __ARGS((char_u *name));
int mch_dirname __ARGS((char_u *buf, int len));
int mch_remove __ARGS((char_u *name));
char_u *mch_getenv __ARGS((char_u *name));
int mch_get_user_name __ARGS((char_u *s, int len));
void mch_get_host_name __ARGS((char_u *s, int len));
/* vim: set ft=c : */
