/**
 ********************************************************************
 *
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 * All information contained herein is, and remains, the property of DJI.
 * The intellectual and technical concepts contained herein are proprietary
 * to DJI and may be covered by U.S. and foreign patents, patents in process,
 * and protected by trade secret or copyright law.  Dissemination of this
 * information, including but not limited to data and other proprietary
 * material(s) incorporated within the information, in any form, is strictly
 * prohibited without the express written consent of DJI.
 *
 * If you receive this source code without DJI's authorization, you may not
 * further disseminate the information, and you must immediately remove the
 * source code and notify DJI of its removal. DJI reserves the right to pursue
 * legal actions against you for any loss(es) or damage(s) caused by your
 * failure to do so.
 *
 *********************************************************************
 */
#ifndef __APP_INFO_H__
#define __APP_INFO_H__

/*
 * Fill in the credentials obtained from the DJI Developer website
 * (Application registration). They are used to authenticate the MQTT
 * connection to the DJI device.
 *
 *   MQTT account  = USER_APP_ID "_esdk_" USER_DEVICE_ID
 *   MQTT password = USER_APP_KEY "_" USER_APP_LICENSE
 *   MQTT ClientID = MQTT account
 */

/* Application info (from the DJI Developer website, same as ESDK 1.x). */
#define USER_APP_NAME           "your_app_name"
#define USER_APP_ID             "your_app_id"
#define USER_APP_KEY            "your_app_key"
#define USER_APP_LICENSE        "your_app_license"
#define USER_DEVELOPER_ACCOUNT  "your_developer_account"

/* Device serial number, used to build the MQTT account / ClientID (ESDK 2.0). */
#define USER_DEVICE_ID          "your_device_id"

/*
 * Optional username suffix.
 *
 * By default the DJI device limits a single message payload to 2048 bytes.
 * Some device types whose single message can exceed this limit may append a
 * suffix to the MQTT username to lift the limit. Leave it empty ("") for the
 * common case.
 *
 * Example: RID devices set this to "@rid" because a single RID event can reach
 * ~20 KB. The chosen suffix must match the device type declared in the
 * capability set.
 */
#define USER_USERNAME_SUFFIX    ""

#endif
