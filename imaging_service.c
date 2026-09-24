/*
 * Copyright (c) 2026 roleo.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "imaging_service.h"
#include "fault.h"
#include "utils.h"
#include "log.h"
#include "ezxml_wrapper.h"
#include "onvif_simple_server.h"

extern service_context_t service_ctx;

/* Focus position is expressed in the fixed generic unitless range. */
int imaging_supports_focus()
{
    if (service_ctx.imaging_node.enable == 0) {
        return 0;
    }
    if (service_ctx.imaging_node.focus_move_in != NULL || service_ctx.imaging_node.focus_move_out != NULL ||
            service_ctx.imaging_node.focus_jump_to_abs != NULL || service_ctx.imaging_node.focus_jump_to_rel != NULL) {
        return 1;
    }
    return 0;
}

/* Returns 1 if the VideoSourceToken in the request matches the device token. */
static int check_video_source_token()
{
    const char *token = get_element("VideoSourceToken", "Body");
    if ((token == NULL) || (strcmp("VideoSourceToken", token) != 0)) {
        return 0;
    }
    return 1;
}

int imaging_get_service_capabilities()
{
    long size = cat(NULL, "imaging_service_files/GetServiceCapabilities.xml", 0);

    output_http_headers(size);

    return cat("stdout", "imaging_service_files/GetServiceCapabilities.xml", 0);
}

int imaging_get_imaging_settings()
{
    char auto_focus_mode[8];
    char ir_cut_filter[8];

    if (service_ctx.imaging_node.enable == 0) {
        send_action_not_supported_fault("imaging_service");
        return -1;
    }
    if (!check_video_source_token()) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:NoSource", "No source", "The requested VideoSource does not exist");
        return -2;
    }

    if (service_ctx.imaging_node.auto_focus == 1) {
        snprintf(auto_focus_mode, sizeof(auto_focus_mode), "%s", "AUTO");
    } else {
        snprintf(auto_focus_mode, sizeof(auto_focus_mode), "%s", "MANUAL");
    }
    if (service_ctx.imaging_node.ir_cut_filter == IRCUT_ON) {
        snprintf(ir_cut_filter, sizeof(ir_cut_filter), "%s", "ON");
    } else if (service_ctx.imaging_node.ir_cut_filter == IRCUT_OFF) {
        snprintf(ir_cut_filter, sizeof(ir_cut_filter), "%s", "OFF");
    } else {
        snprintf(ir_cut_filter, sizeof(ir_cut_filter), "%s", "AUTO");
    }

    long size = cat(NULL, "imaging_service_files/GetImagingSettings.xml", 4,
            "%AUTOFOCUS_MODE%", auto_focus_mode,
            "%IRCUT_FILTER%", ir_cut_filter);

    output_http_headers(size);

    return cat("stdout", "imaging_service_files/GetImagingSettings.xml", 4,
            "%AUTOFOCUS_MODE%", auto_focus_mode,
            "%IRCUT_FILTER%", ir_cut_filter);
}

int imaging_set_imaging_settings()
{
    const char *af;
    const char *ircut;
    char sys_command[MAX_LEN];

    if (service_ctx.imaging_node.enable == 0) {
        send_action_not_supported_fault("imaging_service");
        return -1;
    }
    if (!check_video_source_token()) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:NoSource", "No source", "The requested VideoSource does not exist");
        return -2;
    }

    af = get_element("AutoFocusMode", "Body");
    if (af != NULL) {
        if (strcasecmp(af, "auto") == 0) {
            if (service_ctx.imaging_node.focus_set_auto_focus == NULL) {
                send_action_failed_fault("imaging_service", -3);
                return -3;
            }
            snprintf(sys_command, sizeof(sys_command), service_ctx.imaging_node.focus_set_auto_focus, "AUTO");
            system(sys_command);
            service_ctx.imaging_node.auto_focus = 1;
        } else if (strcasecmp(af, "manual") == 0) {
            if (service_ctx.imaging_node.focus_set_auto_focus == NULL) {
                send_action_failed_fault("imaging_service", -4);
                return -4;
            }
            snprintf(sys_command, sizeof(sys_command), service_ctx.imaging_node.focus_set_auto_focus, "MANUAL");
            system(sys_command);
            service_ctx.imaging_node.auto_focus = 0;
        } else {
            send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidImagingSettings", "Invalid imaging settings", "The requested AutoFocusMode is not supported");
            return -5;
        }
    }

    ircut = get_element("IrCutFilter", "Body");
    if (ircut != NULL) {
        if (service_ctx.imaging_node.ir_cut_filter_set == NULL) {
            send_action_failed_fault("imaging_service", -6);
            return -6;
        }
        if (strcasecmp(ircut, "auto") == 0) {
            snprintf(sys_command, sizeof(sys_command), service_ctx.imaging_node.ir_cut_filter_set, "AUTO");
            service_ctx.imaging_node.ir_cut_filter = IRCUT_AUTO;
        } else if (strcasecmp(ircut, "on") == 0) {
            snprintf(sys_command, sizeof(sys_command), service_ctx.imaging_node.ir_cut_filter_set, "ON");
            service_ctx.imaging_node.ir_cut_filter = IRCUT_ON;
        } else if (strcasecmp(ircut, "off") == 0) {
            snprintf(sys_command, sizeof(sys_command), service_ctx.imaging_node.ir_cut_filter_set, "OFF");
            service_ctx.imaging_node.ir_cut_filter = IRCUT_OFF;
        } else {
            send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidImagingSettings", "Invalid imaging settings", "The requested IrCutFilter is not supported");
            return -7;
        }
        system(sys_command);
    }

    if ((af == NULL) && (ircut == NULL)) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidImagingSettings", "Invalid imaging settings", "No imageable setting provided");
        return -8;
    }

    long size = cat(NULL, "imaging_service_files/SetImagingSettings.xml", 0);

    output_http_headers(size);

    return cat("stdout", "imaging_service_files/SetImagingSettings.xml", 0);
}

int imaging_get_options()
{
    char min_focus[32], max_focus[32];
    char ir_cut_filter_options[64];

    if (service_ctx.imaging_node.enable == 0) {
        send_action_not_supported_fault("imaging_service");
        return -1;
    }
    if (!check_video_source_token()) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:NoSource", "No source", "The requested VideoSource does not exist");
        return -2;
    }

    snprintf(min_focus, sizeof(min_focus), "%.1f", service_ctx.imaging_node.focus_min_step);
    snprintf(max_focus, sizeof(max_focus), "%.1f", service_ctx.imaging_node.focus_max_step);
    if (service_ctx.imaging_node.ir_cut_filter_set == NULL) {
        snprintf(ir_cut_filter_options, sizeof(ir_cut_filter_options), "%s", "");
    } else {
        snprintf(ir_cut_filter_options, sizeof(ir_cut_filter_options), "%s",
                "<tt:IrCutFilterModes>ON</tt:IrCutFilterModes><tt:IrCutFilterModes>OFF</tt:IrCutFilterModes><tt:IrCutFilterModes>AUTO</tt:IrCutFilterModes>");
    }

    long size = cat(NULL, "imaging_service_files/GetOptions.xml", 4,
            "%MIN_FOCUS%", min_focus,
            "%MAX_FOCUS%", max_focus,
            "%IRCUT_FILTER_MODES%", ir_cut_filter_options);

    output_http_headers(size);

    return cat("stdout", "imaging_service_files/GetOptions.xml", 4,
            "%MIN_FOCUS%", min_focus,
            "%MAX_FOCUS%", max_focus,
            "%IRCUT_FILTER_MODES%", ir_cut_filter_options);
}

int imaging_get_move_options()
{
    char min_focus[32], max_focus[32];

    if (service_ctx.imaging_node.enable == 0) {
        send_action_not_supported_fault("imaging_service");
        return -1;
    }
    if (!check_video_source_token()) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:NoSource", "No source", "The requested VideoSource does not exist");
        return -2;
    }

    snprintf(min_focus, sizeof(min_focus), "%.1f", service_ctx.imaging_node.focus_min_step);
    snprintf(max_focus, sizeof(max_focus), "%.1f", service_ctx.imaging_node.focus_max_step);

    long size = cat(NULL, "imaging_service_files/GetMoveOptions.xml", 2,
            "%MIN_FOCUS%", min_focus,
            "%MAX_FOCUS%", max_focus);

    output_http_headers(size);

    return cat("stdout", "imaging_service_files/GetMoveOptions.xml", 2,
            "%MIN_FOCUS%", min_focus,
            "%MAX_FOCUS%", max_focus);
}

int imaging_move()
{
    const char *speed = NULL;
    const char *distance = NULL;
    const char *position = NULL;
    double dspeed, ddist, dpos;
    char sys_command[MAX_LEN];
    ezxml_t node, focus_node;

    if (service_ctx.imaging_node.enable == 0) {
        send_action_not_supported_fault("imaging_service");
        return -1;
    }
    if (!check_video_source_token()) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:NoSource", "No source", "The requested VideoSource does not exist");
        return -2;
    }

    if (!imaging_supports_focus()) {
        send_action_not_supported_fault("imaging_service");
        return -3;
    }

    node = get_element_ptr(NULL, "Focus", "Body");
    if (node == NULL) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidArgVal", "Invalid argument value", "The Mask/ConfigurationToken element is invalid");
        return -4;
    }

    focus_node = get_element_in_element_ptr("Continuous", node);
    if (focus_node != NULL) {
        speed = get_element_in_element("Speed", focus_node);
        if (speed != NULL) {
            dspeed = atof(speed);
            if (dspeed > 0.0) {
                if (service_ctx.imaging_node.focus_move_in == NULL) {
                    send_action_failed_fault("imaging_service", -5);
                    return -5;
                }
                snprintf(sys_command, sizeof(sys_command), service_ctx.imaging_node.focus_move_in, dspeed);
                system(sys_command);
            } else if (dspeed < 0.0) {
                if (service_ctx.imaging_node.focus_move_out == NULL) {
                    send_action_failed_fault("imaging_service", -6);
                    return -6;
                }
                snprintf(sys_command, sizeof(sys_command), service_ctx.imaging_node.focus_move_out, -dspeed);
                system(sys_command);
            }
        } else {
            send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidArgVal", "Invalid argument value", "The Speed element is required for continuous moves");
            return -7;
        }
    }

    focus_node = get_element_in_element_ptr("Relative", node);
    if (focus_node != NULL) {
        distance = get_element_in_element("Distance", focus_node);
        if (distance != NULL) {
            ddist = atof(distance);
            if ((ddist > service_ctx.imaging_node.focus_max_step) || (ddist < -service_ctx.imaging_node.focus_max_step)) {
                send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidArgVal", "Invalid argument value", "The requested focus distance is out of bounds");
                return -8;
            }
            if (service_ctx.imaging_node.focus_jump_to_rel == NULL) {
                send_action_failed_fault("imaging_service", -9);
                return -9;
            }
            snprintf(sys_command, sizeof(sys_command), service_ctx.imaging_node.focus_jump_to_rel, ddist);
            system(sys_command);
        } else {
            send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidArgVal", "Invalid argument value", "The Distance element is required for relative moves");
            return -10;
        }
    }

    focus_node = get_element_in_element_ptr("Absolute", node);
    if (focus_node != NULL) {
        position = get_element_in_element("Position", focus_node);
        if (position != NULL) {
            dpos = atof(position);
            if ((dpos > service_ctx.imaging_node.focus_max_step) || (dpos < service_ctx.imaging_node.focus_min_step)) {
                send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidArgVal", "Invalid argument value", "The requested focus position is out of bounds");
                return -11;
            }
            if (service_ctx.imaging_node.focus_jump_to_abs == NULL) {
                send_action_failed_fault("imaging_service", -12);
                return -12;
            }
            snprintf(sys_command, sizeof(sys_command), service_ctx.imaging_node.focus_jump_to_abs, dpos);
            system(sys_command);
        } else {
            send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidArgVal", "Invalid argument value", "The Position element is required for absolute moves");
            return -13;
        }
    }

    if ((speed == NULL) && (distance == NULL) && (position == NULL)) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:InvalidArgVal", "Invalid argument value", "No focus move operation provided");
        return -14;
    }

    long size = cat(NULL, "imaging_service_files/Move.xml", 0);

    output_http_headers(size);

    return cat("stdout", "imaging_service_files/Move.xml", 0);
}

int imaging_stop()
{
    char sys_command[MAX_LEN];

    if (service_ctx.imaging_node.enable == 0) {
        send_action_not_supported_fault("imaging_service");
        return -1;
    }
    if (!check_video_source_token()) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:NoSource", "No source", "The requested VideoSource does not exist");
        return -2;
    }

    if (service_ctx.imaging_node.focus_move_stop == NULL) {
        send_action_failed_fault("imaging_service", -3);
        return -3;
    }

    snprintf(sys_command, sizeof(sys_command), "%s", service_ctx.imaging_node.focus_move_stop);
    system(sys_command);

    long size = cat(NULL, "imaging_service_files/Stop.xml", 0);

    output_http_headers(size);

    return cat("stdout", "imaging_service_files/Stop.xml", 0);
}

int imaging_get_status()
{
    char utctime[32];
    time_t timestamp = time(NULL);
    struct tm *tm = gmtime(&timestamp);
    int ret = 0;
    int i = 0;
    char out[256], state[16];

    if (service_ctx.imaging_node.enable == 0) {
        send_action_not_supported_fault("imaging_service");
        return -1;
    }
    if (!check_video_source_token()) {
        send_fault("imaging_service", "Sender", "ter:InvalidArgVal", "ter:NoSource", "No source", "The requested VideoSource does not exist");
        return -2;
    }

    snprintf(utctime, sizeof(utctime), "%04d-%02d-%02dT%02d:%02d:%02dZ",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);

    if (service_ctx.imaging_node.focus_get_position != NULL) {
        if (spawn_capture(service_ctx.imaging_node.focus_get_position, out, sizeof(out), 2) <= 0) {
            ret = -4;
        }
    } else {
        ret = -5;
    }

    if (service_ctx.imaging_node.focus_is_moving != NULL) {
        if (spawn_capture(service_ctx.imaging_node.focus_is_moving, out, sizeof(out), 2) <= 0) {
            ret = -6;
        } else if (sscanf(out, "%d", &i) < 1) {
            ret = -7;
        }
    } else {
        i = 0;
    }

    if (ret == 0) {
        if (i == 1) {
            snprintf(state, sizeof(state), "%s", "MOVING");
        } else {
            snprintf(state, sizeof(state), "%s", "IDLE");
        }

        long size = cat(NULL, "imaging_service_files/GetStatus.xml", 2,
                "%FOCUS_STATE%", state,
                "%TIME%", utctime);

        output_http_headers(size);

        return cat("stdout", "imaging_service_files/GetStatus.xml", 2,
                "%FOCUS_STATE%", state,
                "%TIME%", utctime);
    } else {
        send_fault("imaging_service", "Receiver", "ter:Action", "ter:NoStatus", "No status", "No imaging status is available for the requested VideoSource");
        return ret;
    }
}

int imaging_unsupported(const char *method)
{
    /* An unimplemented action must return a SOAP fault, not an empty 200
     * response (ONVIF: env:Receiver / ter:ActionNotSupported). */
    (void) method;
    return send_action_not_supported_fault("imaging_service");
}