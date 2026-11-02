/* generated: the state of the editor (see tdestate.h) */

enum {
   TDE_MOD_ED,
   TDE_MOD_UTILS,
   TDE_MOD_MAIN,
   TDE_MOD_BLOCK,
   TDE_MOD_CONFIG,
   TDE_MOD_DIRLIST,
   TDE_MOD_FILE,
   TDE_MOD_FILMATCH,
   TDE_MOD_FINDREP,
   TDE_MOD_MACRO,
   TDE_MOD_MEMORY,
   TDE_MOD_PULL,
   TDE_MOD_QUERY,
   TDE_MOD_REGX,
   TDE_MOD_SYNTAX,
   TDE_MOD_COUNT
};

#define TDE_SHARED_MEMBERS \
   DIALOG border_dialog[18]; \
   DIALOG diff_dialog[11]; \
   DIALOG exec_dialog[9]; \
   DIALOG fattr_dialog[16]; \
   DIALOG find_dialog[10]; \
   DIALOG grep_dialog[10]; \
   DIALOG margins_dialog[10]; \
   DIALOG number_dialog[10]; \
   DIALOG replace_dialog[12]; \
   DIALOG tabs_dialog[7]; \
   long swap_br; \
   long swap_er; \
   int swap_bc; \
   int swap_ec; \
   char *line_in; \
   int line_no; \
   int process_macro; \
   MACRO *mac; \
   TREE *cfg_key_tree; \
   int background; \
   char *cmd_config; \
   MENU_STR *user_menu; \
   KEY_FUNC key_func; \
   jmp_buf editor_jmp; \
   char wksp_file[PATH_MAX]; \
   int wksp_loaded; \
   int block_replace_offset; \
   long last_replace_line; \
   int search_type; \
   TDE_WIN *results_window; \
   file_infos *results_file; \
   file_infos *search_file; \
   long found_rline; \
   int found_rcol; \
   int found_len; \
   int found_vlen; \
   displays g_display; \
   status_infos g_status; \
   boyer_moore_type bm; \
   boyer_moore_type sas_bm; \
   MACRO *macro[MODIFIERS][MAX_KEYS]; \
   TREE key_tree; \
   SORT sort; \
   DIFF diff; \
   REGX_INFO regx; \
   REGX_INFO sas_regx; \
   NFA_TYPE nfa; \
   NFA_TYPE sas_nfa; \
   char line_out[MAX_COLS+2]; \
   chtype tde_color_table[256]; \
   char ruler_line[MAX_LINE_LENGTH+MAX_COLS+2]; \
   TDE_WIN ruler_win; \
   int scancode_map[MAX_KEYS]; \
   option_infos g_option_all; \
   option_infos g_option; \
   mode_infos mode; \
   HISTORY h_file; \
   HISTORY h_find; \
   HISTORY h_border; \
   HISTORY h_fill; \
   HISTORY h_stamp; \
   HISTORY h_line; \
   HISTORY h_lang; \
   HISTORY h_grep; \
   HISTORY h_win; \
   HISTORY h_exec; \
   HISTORY h_parm; \
   TDE_COLORS colour; \
   int syntax_color[SHL_NUM_COLORS]; \
   char *stat_screen[STATS_COUNT+1]; \
   int help_dim[2][2]; \
   char init_wd[PATH_MAX]; \
   TDE_WIN *browser_window; \
   int nfa_status; \
   int subs_pos[10]; \
   int subs_len[10]; \
   int copied_rcol; \
   int copied_mod; \
   int copied_dirty; \
   int auto_reload; \
   int make_window; \



void block_state_init( void );
void config_state_init( void );
void default_state_init( void );
void dirlist_state_init( void );
void file_state_init( void );
void filmatch_state_init( void );
void findrep_state_init( void );
void global_state_init( void );
void dialogs_state_init( void );
void menu_state_init( void );
void ed_state_init( void );
void utils_state_init( void );
void main_state_init( void );
void help_state_init( void );
void macro_state_init( void );
void memory_state_init( void );
void pull_state_init( void );
void query_state_init( void );
void regx_state_init( void );
void syntax_state_init( void );
#define TDE_STATE_INITS \
   menu_state_init( ); \
   ed_state_init( ); \
   utils_state_init( ); \
   main_state_init( ); \
   dialogs_state_init( ); \
   block_state_init( ); \
   config_state_init( ); \
   default_state_init( ); \
   dirlist_state_init( ); \
   file_state_init( ); \
   filmatch_state_init( ); \
   findrep_state_init( ); \
   global_state_init( ); \
   help_state_init( ); \
   macro_state_init( ); \
   memory_state_init( ); \
   pull_state_init( ); \
   query_state_init( ); \
   regx_state_init( ); \
   syntax_state_init( ); \

