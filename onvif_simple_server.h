/*
 * Copyright (c) 2024 roleo.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ONVIF_SIMPLE_SERVER_H
#define ONVIF_SIMPLE_SERVER_H

#define DAEMON_NO_CHDIR          01 /* Don't chdir ("/") */
#define DAEMON_NO_CLOSE_FILES    02 /* Don't close all open files */
#define DAEMON_NO_REOPEN_STD_FDS 04 /* Don't reopen stdin, stdout, and stderr to /dev/null */
#define DAEMON_NO_UMASK0        010 /* Don't do a umask(0) */
#define DAEMON_MAX_CLOSE       8192 /* Max file descriptors to close if sysconf(_SC_OPEN_MAX) is indeterminate */

typedef struct {
    int enable;
    const char *username;
    const char *password;
    const char *nonce;
    const char *created;
    int  type;
} username_token_t;

typedef enum {
    VIDEO_NONE,
    JPEG,
    MPEG4,
    H264,
    H265
} stream_type;

typedef enum {
    AUDIO_NONE,
    G711,
    G726,
    AAC
} audio_type;

typedef enum {
    IDLE_STATE_CLOSE,
    IDLE_STATE_OPEN
} idle_state;

typedef struct {
    char *name;
    int  width;
    int  height;
    char *url;
    char *snapurl;
    int  type;
    int  bitrate;       /* kbps, 0 = unknown */
    int  framerate;     /* fps, 0 = unknown */
    int  audio_encoder;
    int  audio_decoder;
} stream_profile_t;

typedef struct {
    int idle_state;
    char *close;
    char *open;
} relay_output_t;

typedef struct {
    char *command;
    char *exec;
} aux_command_t;

/* Bumped from upstream's 8: the web panel is a generic front end for every
 * aux channel, so spare channels are worth advertising.  This only gates the
 * two bounds checks in conf.c (the array is heap-allocated), but it is NOT the
 * real ceiling - GetNode substitutes the whole block into one MAX_CAT_LEN
 * (2048) line, at ~68 bytes + label per command.  12 entries stay under that
 * even with 60-char labels. */
#define MAX_AUX_COMMANDS 12

typedef enum {
    IRCUT_AUTO,
    IRCUT_ON,
    IRCUT_OFF
} ir_cut_filter_t;

typedef struct {
    int enable;
    int zoom_enable;
    double min_step_x;
    double max_step_x;
    double min_step_y;
    double max_step_y;
    double min_step_z;
    double max_step_z;
    char *get_position;
    char *is_moving;
    char *move_left;
    char *move_right;
    char *move_up;
    char *move_down;
    char *move_in;
    char *move_out;
    char *move_stop;
    char *move_preset;
    char *goto_home_position;
    char *set_preset;
    char *set_home_position;
    char *remove_preset;
    char *jump_to_abs;
    char *jump_to_rel;
    char *get_presets;
} ptz_node_t;

typedef struct {
    int enable;
    double focus_min_step;
    double focus_max_step;
    char *focus_move_in;
    char *focus_move_out;
    char *focus_move_stop;
    char *focus_jump_to_abs;
    char *focus_jump_to_rel;
    char *focus_get_position;
    char *focus_is_moving;
    char *focus_set_auto_focus;
    int auto_focus;              /* 0 = MANUAL, 1 = AUTO */
    ir_cut_filter_t ir_cut_filter;
    char *ir_cut_filter_set;
} imaging_node_t;

typedef struct {
    char *topic;
    char *source_name;
    char *source_type;
    char *source_value;
    char *input_file;
} event_t;

typedef struct {
    int   port;
    char *user;
    char *password;

    //Device Information
    char *manufacturer;
    char *model;
    char *firmware_ver;
    char *serial_num;
    char *hardware_id;

    char device_uuid[37];   /* stable MAC-based UUID v5 */
    char address[46];       /* resolved local IP (plain, no scope ID) */
    char address_url[48];   /* URL-safe: IPv6 wrapped in [...] */

    char *ifs;

    int adv_enable_media2;
    int adv_fault_if_unknown;
    int adv_fault_if_set;
    int adv_synology_nvr;

    char *factory_default_command;   /* SetSystemFactoryDefault hook; NULL = op not supported */

    stream_profile_t *profiles;
    int profiles_num;

    char **scopes;
    int scopes_num;

    relay_output_t *relay_outputs;
    int relay_outputs_num;
    ptz_node_t ptz_node;
    aux_command_t *aux_commands;
    int aux_commands_num;
    imaging_node_t imaging_node;
    event_t *events;
    int events_enable;
    int events_num;
} service_context_t;

// returns 0 on success -1 on error
int daemonize(int flags);

#endif //ONVIF_SIMPLE_SERVER_H
