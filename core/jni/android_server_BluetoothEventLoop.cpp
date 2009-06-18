/*
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "BluetoothEventLoop.cpp"

#include "android_bluetooth_common.h"
#include "android_runtime/AndroidRuntime.h"
#include "cutils/sockets.h"
#include "JNIHelp.h"
#include "jni.h"
#include "utils/Log.h"
#include "utils/misc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#ifdef HAVE_BLUETOOTH
#include <dbus/dbus.h>
#endif

namespace android {

#ifdef HAVE_BLUETOOTH
static jfieldID field_mNativeData;

static jmethodID method_onPropertyChanged;
static jmethodID method_onDevicePropertyChanged;
static jmethodID method_onDeviceFound;
static jmethodID method_onDeviceDisappeared;
static jmethodID method_onDeviceCreated;
static jmethodID method_onDeviceRemoved;

static jmethodID method_onCreatePairedDeviceResult;
static jmethodID method_onGetDeviceServiceChannelResult;

static jmethodID method_onRequestPinCode;
static jmethodID method_onAgentAuthorize;
static jmethodID method_onAgentCancel;

typedef event_loop_native_data_t native_data_t;

static inline native_data_t * get_native_data(JNIEnv *env, jobject object) {
    return (native_data_t *)(env->GetIntField(object,
                                                 field_mNativeData));
}

native_data_t *get_EventLoop_native_data(JNIEnv *env, jobject object) {
    return get_native_data(env, object);
}

#endif
static void classInitNative(JNIEnv* env, jclass clazz) {
    LOGV(__FUNCTION__);

#ifdef HAVE_BLUETOOTH
    method_onPropertyChanged = env->GetMethodID(clazz, "onPropertyChanged",
                                                "([Ljava/lang/String;)V");
    method_onDevicePropertyChanged = env->GetMethodID(clazz,
                                                      "onDevicePropertyChanged",
                                                      "(Ljava/lang/String;[Ljava/lang/String;)V");
    method_onDeviceFound = env->GetMethodID(clazz, "onDeviceFound",
                                            "(Ljava/lang/String;[Ljava/lang/String;)V");
    method_onDeviceDisappeared = env->GetMethodID(clazz, "onDeviceDisappeared",
                                                  "(Ljava/lang/String;)V");
    method_onDeviceCreated = env->GetMethodID(clazz, "onDeviceCreated", "(Ljava/lang/String;)V");
    method_onDeviceRemoved = env->GetMethodID(clazz, "onDeviceRemoved", "(Ljava/lang/String;)V");

    method_onCreatePairedDeviceResult = env->GetMethodID(clazz, "onCreatePairedDeviceResult",
                                                         "(Ljava/lang/String;I)V");

    method_onAgentAuthorize = env->GetMethodID(clazz, "onAgentAuthorize",
                                               "(Ljava/lang/String;Ljava/lang/String;)Z");
    method_onAgentCancel = env->GetMethodID(clazz, "onAgentCancel", "()V");
    method_onRequestPinCode = env->GetMethodID(clazz, "onRequestPinCode",
                                               "(Ljava/lang/String;I)V");

    field_mNativeData = env->GetFieldID(clazz, "mNativeData", "I");
#endif
}

static void initializeNativeDataNative(JNIEnv* env, jobject object) {
    LOGV(__FUNCTION__);
#ifdef HAVE_BLUETOOTH
    native_data_t *nat = (native_data_t *)calloc(1, sizeof(native_data_t));
    if (NULL == nat) {
        LOGE("%s: out of memory!", __FUNCTION__);
        return;
    }
    memset(nat, 0, sizeof(native_data_t));

    pthread_mutex_init(&(nat->thread_mutex), NULL);

    env->SetIntField(object, field_mNativeData, (jint)nat);

    {
        DBusError err;
        dbus_error_init(&err);
        dbus_threads_init_default();
        nat->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
        if (dbus_error_is_set(&err)) {
            LOGE("%s: Could not get onto the system bus!", __FUNCTION__);
            dbus_error_free(&err);
        }
        dbus_connection_set_exit_on_disconnect(nat->conn, FALSE);
    }
#endif
}

static void cleanupNativeDataNative(JNIEnv* env, jobject object) {
    LOGV(__FUNCTION__);
#ifdef HAVE_BLUETOOTH
    native_data_t *nat =
            (native_data_t *)env->GetIntField(object, field_mNativeData);

    pthread_mutex_destroy(&(nat->thread_mutex));

    if (nat) {
        free(nat);
    }
#endif
}

#ifdef HAVE_BLUETOOTH
static DBusHandlerResult event_filter(DBusConnection *conn, DBusMessage *msg,
                                      void *data);
DBusHandlerResult agent_event_filter(DBusConnection *conn,
                                     DBusMessage *msg,
                                     void *data);
static int register_agent(native_data_t *nat,
                          const char *agent_path, const char *capabilities);

static const DBusObjectPathVTable agent_vtable = {
    NULL, agent_event_filter, NULL, NULL, NULL, NULL
};


static jboolean setUpEventLoop(native_data_t *nat) {
    LOGV(__FUNCTION__);

    if (nat != NULL && nat->conn != NULL) {
        dbus_threads_init_default();
        DBusError err;
        dbus_error_init(&err);

        // Add a filter for all incoming messages
        if (!dbus_connection_add_filter(nat->conn, event_filter, nat, NULL)){
            return JNI_FALSE;
        }

        // Set which messages will be processed by this dbus connection
        dbus_bus_add_match(nat->conn,
                "type='signal',interface='org.freedesktop.DBus'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
            return JNI_FALSE;
        }
        dbus_bus_add_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".Adapter'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
            return JNI_FALSE;
        }
        dbus_bus_add_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".Device'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
            return JNI_FALSE;
        }
        dbus_bus_add_match(nat->conn,
                "type='signal',interface='org.bluez.AudioSink'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
            return JNI_FALSE;
        }

        const char *agent_path = "/android/bluetooth/agent";
        const char *capabilities = "DisplayYesNo";
        if (register_agent(nat, agent_path, capabilities) < 0) {
            dbus_connection_unregister_object_path (nat->conn, agent_path);
            return JNI_FALSE;
        }
        return JNI_TRUE;
    }
    return JNI_FALSE;
}


const char * get_adapter_path(DBusConnection *conn) {
    DBusMessage *msg, *reply;
    DBusError err;
    const char *device_path = NULL;
    msg = dbus_message_new_method_call("org.bluez", "/",
          "org.bluez.Manager", "DefaultAdapter");
    if (!msg) {
        LOGE("%s: Can't allocate new method call for GetProperties!",
              __FUNCTION__);
        return NULL;
    }
    dbus_message_append_args(msg, DBUS_TYPE_INVALID);

    dbus_error_init(&err);
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }
        return NULL;
    }
    if (!dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH,
                               &device_path, DBUS_TYPE_INVALID)
                               || !device_path){
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }
        return NULL;
    }
    return device_path;
}

static int register_agent(native_data_t *nat,
                          const char * agent_path, const char * capabilities)
{
    DBusMessage *msg, *reply;
    DBusError err;

    if (!dbus_connection_register_object_path(nat->conn, agent_path,
            &agent_vtable, nat)) {
        LOGE("%s: Can't register object path %s for agent!",
              __FUNCTION__, agent_path);
        return -1;
    }

    nat->adapter = get_adapter_path(nat->conn);
    msg = dbus_message_new_method_call("org.bluez", nat->adapter,
          "org.bluez.Adapter", "RegisterAgent");
    if (!msg) {
        LOGE("%s: Can't allocate new method call for agent!",
              __FUNCTION__);
        return -1;
    }
    dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &agent_path,
                             DBUS_TYPE_STRING, &capabilities,
                             DBUS_TYPE_INVALID);

    dbus_error_init(&err);
    reply = dbus_connection_send_with_reply_and_block(nat->conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (!reply) {
        LOGE("%s: Can't register agent!", __FUNCTION__);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }
        return -1;
    }

    dbus_message_unref(reply);
    dbus_connection_flush(nat->conn);

    return 0;
}

static void tearDownEventLoop(native_data_t *nat) {
    LOGV(__FUNCTION__);
    if (nat != NULL && nat->conn != NULL) {

        DBusMessage *msg, *reply;
        DBusError err;
        dbus_error_init(&err);
        const char * agent_path = "/android/bluetooth/agent";

        msg = dbus_message_new_method_call("org.bluez",
                                           nat->adapter,
                                           "org.bluez.Adapter",
                                           "UnregisterAgent");
        if (msg != NULL) {
            dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &agent_path,
                                     DBUS_TYPE_INVALID);
            reply = dbus_connection_send_with_reply_and_block(nat->conn,
                                                              msg, -1, &err);

            if (!reply) {
                if (dbus_error_is_set(&err)) {
                    LOG_AND_FREE_DBUS_ERROR(&err);
                    dbus_error_free(&err);
                }
            } else {
                dbus_message_unref(reply);
            }
            dbus_message_unref(msg);
        } else {
             LOGE("%s: Can't create new method call!", __FUNCTION__);
        }

        dbus_connection_flush(nat->conn);
        dbus_connection_unregister_object_path(nat->conn, agent_path);

        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='org.bluez.audio.Sink'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }
        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='org.bluez.audio.Device'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }
        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='org.bluez.audio.Manager'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }
        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".Adapter'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }
        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='org.freedesktop.DBus'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }

        dbus_connection_remove_filter(nat->conn, event_filter, nat);
    }
}


#define EVENT_LOOP_EXIT 1
#define EVENT_LOOP_ADD  2
#define EVENT_LOOP_REMOVE 3

dbus_bool_t dbusAddWatch(DBusWatch *watch, void *data) {
    native_data_t *nat = (native_data_t *)data;

    if (dbus_watch_get_enabled(watch)) {
        // note that we can't just send the watch and inspect it later
        // because we may get a removeWatch call before this data is reacted
        // to by our eventloop and remove this watch..  reading the add first
        // and then inspecting the recently deceased watch would be bad.
        char control = EVENT_LOOP_ADD;
        write(nat->controlFdW, &control, sizeof(char));

        int fd = dbus_watch_get_fd(watch);
        write(nat->controlFdW, &fd, sizeof(int));

        unsigned int flags = dbus_watch_get_flags(watch);
        write(nat->controlFdW, &flags, sizeof(unsigned int));

        write(nat->controlFdW, &watch, sizeof(DBusWatch*));
    }
    return true;
}

void dbusRemoveWatch(DBusWatch *watch, void *data) {
    native_data_t *nat = (native_data_t *)data;

    char control = EVENT_LOOP_REMOVE;
    write(nat->controlFdW, &control, sizeof(char));

    int fd = dbus_watch_get_fd(watch);
    write(nat->controlFdW, &fd, sizeof(int));

    unsigned int flags = dbus_watch_get_flags(watch);
    write(nat->controlFdW, &flags, sizeof(unsigned int));
}

void dbusToggleWatch(DBusWatch *watch, void *data) {
    if (dbus_watch_get_enabled(watch)) {
        dbusAddWatch(watch, data);
    } else {
        dbusRemoveWatch(watch, data);
    }
}

static void handleWatchAdd(native_data_t *nat) {
    DBusWatch *watch;
    int newFD;
    unsigned int flags;

    read(nat->controlFdR, &newFD, sizeof(int));
    read(nat->controlFdR, &flags, sizeof(unsigned int));
    read(nat->controlFdR, &watch, sizeof(DBusWatch *));
    int events = (flags & DBUS_WATCH_READABLE ? POLLIN : 0)
            | (flags & DBUS_WATCH_WRITABLE ? POLLOUT : 0);

    for (int y = 0; y<nat->pollMemberCount; y++) {
        if ((nat->pollData[y].fd == newFD) &&
                (nat->pollData[y].events == events)) {
            LOGV("DBusWatch duplicate add");
            return;
        }
    }
    if (nat->pollMemberCount == nat->pollDataSize) {
        LOGV("Bluetooth EventLoop poll struct growing");
        struct pollfd *temp = (struct pollfd *)malloc(
                sizeof(struct pollfd) * (nat->pollMemberCount+1));
        if (!temp) {
            return;
        }
        memcpy(temp, nat->pollData, sizeof(struct pollfd) *
                nat->pollMemberCount);
        free(nat->pollData);
        nat->pollData = temp;
        DBusWatch **temp2 = (DBusWatch **)malloc(sizeof(DBusWatch *) *
                (nat->pollMemberCount+1));
        if (!temp2) {
            return;
        }
        memcpy(temp2, nat->watchData, sizeof(DBusWatch *) *
                nat->pollMemberCount);
        free(nat->watchData);
        nat->watchData = temp2;
        nat->pollDataSize++;
    }
    nat->pollData[nat->pollMemberCount].fd = newFD;
    nat->pollData[nat->pollMemberCount].revents = 0;
    nat->pollData[nat->pollMemberCount].events = events;
    nat->watchData[nat->pollMemberCount] = watch;
    nat->pollMemberCount++;
}

static void handleWatchRemove(native_data_t *nat) {
    int removeFD;
    unsigned int flags;

    read(nat->controlFdR, &removeFD, sizeof(int));
    read(nat->controlFdR, &flags, sizeof(unsigned int));
    int events = (flags & DBUS_WATCH_READABLE ? POLLIN : 0)
            | (flags & DBUS_WATCH_WRITABLE ? POLLOUT : 0);

    for (int y = 0; y < nat->pollMemberCount; y++) {
        if ((nat->pollData[y].fd == removeFD) &&
                (nat->pollData[y].events == events)) {
            int newCount = --nat->pollMemberCount;
            // copy the last live member over this one
            nat->pollData[y].fd = nat->pollData[newCount].fd;
            nat->pollData[y].events = nat->pollData[newCount].events;
            nat->pollData[y].revents = nat->pollData[newCount].revents;
            nat->watchData[y] = nat->watchData[newCount];
            return;
        }
    }
    LOGW("WatchRemove given with unknown watch");
}

static void *eventLoopMain(void *ptr) {
    native_data_t *nat = (native_data_t *)ptr;
    JNIEnv *env;

    JavaVMAttachArgs args;
    char name[] = "BT EventLoop";
    args.version = nat->envVer;
    args.name = name;
    args.group = NULL;

    nat->vm->AttachCurrentThread(&env, &args);

    dbus_connection_set_watch_functions(nat->conn, dbusAddWatch,
            dbusRemoveWatch, dbusToggleWatch, ptr, NULL);

    while (1) {
        for (int i = 0; i < nat->pollMemberCount; i++) {
            if (!nat->pollData[i].revents) {
                continue;
            }
            if (nat->pollData[i].fd == nat->controlFdR) {
                char data;
                while (recv(nat->controlFdR, &data, sizeof(char), MSG_DONTWAIT)
                        != -1) {
                    switch (data) {
                    case EVENT_LOOP_EXIT:
                    {
                        dbus_connection_set_watch_functions(nat->conn,
                                NULL, NULL, NULL, NULL, NULL);
                        tearDownEventLoop(nat);
                        nat->vm->DetachCurrentThread();
                        shutdown(nat->controlFdR,SHUT_RDWR);
                        return NULL;
                    }
                    case EVENT_LOOP_ADD:
                    {
                        handleWatchAdd(nat);
                        break;
                    }
                    case EVENT_LOOP_REMOVE:
                    {
                        handleWatchRemove(nat);
                        break;
                    }
                    }
                }
            } else {
                  int event = nat->pollData[i].revents;
                  int flags = (event & POLLIN ? DBUS_WATCH_READABLE : 0) |
                              (event & POLLOUT ? DBUS_WATCH_WRITABLE : 0);
                  dbus_watch_handle(nat->watchData[i], event);
                  nat->pollData[i].revents = 0;
                  // can only do one - it may have caused a 'remove'
                  break;
            }
        }
        while (dbus_connection_dispatch(nat->conn) == 
                DBUS_DISPATCH_DATA_REMAINS) {
        }

        poll(nat->pollData, nat->pollMemberCount, -1);
    }
}
#endif // HAVE_BLUETOOTH

static jboolean startEventLoopNative(JNIEnv *env, jobject object) {
    jboolean result = JNI_FALSE;
#ifdef HAVE_BLUETOOTH
    event_loop_native_data_t *nat = get_native_data(env, object);

    pthread_mutex_lock(&(nat->thread_mutex));

    if (nat->pollData) {
        LOGW("trying to start EventLoop a second time!");
        pthread_mutex_unlock( &(nat->thread_mutex) );
        return JNI_FALSE;
    }

    nat->pollData = (struct pollfd *)malloc(sizeof(struct pollfd) *
            DEFAULT_INITIAL_POLLFD_COUNT);
    if (!nat->pollData) {
        LOGE("out of memory error starting EventLoop!");
        goto done;
    }

    nat->watchData = (DBusWatch **)malloc(sizeof(DBusWatch *) *
            DEFAULT_INITIAL_POLLFD_COUNT);
    if (!nat->watchData) {
        LOGE("out of memory error starting EventLoop!");
        goto done;
    }

    memset(nat->pollData, 0, sizeof(struct pollfd) *
            DEFAULT_INITIAL_POLLFD_COUNT);
    memset(nat->watchData, 0, sizeof(DBusWatch *) *
            DEFAULT_INITIAL_POLLFD_COUNT);
    nat->pollDataSize = DEFAULT_INITIAL_POLLFD_COUNT;
    nat->pollMemberCount = 1;

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, &(nat->controlFdR))) {
        LOGE("Error getting BT control socket");
        goto done;
    }
    nat->pollData[0].fd = nat->controlFdR;
    nat->pollData[0].events = POLLIN;

    env->GetJavaVM( &(nat->vm) );
    nat->envVer = env->GetVersion();

    nat->me = env->NewGlobalRef(object);

    if (setUpEventLoop(nat) != JNI_TRUE) {
        LOGE("failure setting up Event Loop!");
        goto done;
    }

    pthread_create(&(nat->thread), NULL, eventLoopMain, nat);
    result = JNI_TRUE;

done:
    if (JNI_FALSE == result) {
        if (nat->controlFdW || nat->controlFdR) {
            shutdown(nat->controlFdW, SHUT_RDWR);
            nat->controlFdW = 0;
            nat->controlFdR = 0;
        }
        if (nat->me) env->DeleteGlobalRef(nat->me);
        nat->me = NULL;
        if (nat->pollData) free(nat->pollData);
        nat->pollData = NULL;
        if (nat->watchData) free(nat->watchData);
        nat->watchData = NULL;
        nat->pollDataSize = 0;
        nat->pollMemberCount = 0;
    }

    pthread_mutex_unlock(&(nat->thread_mutex));
#endif // HAVE_BLUETOOTH
    return result;
}

static void stopEventLoopNative(JNIEnv *env, jobject object) {
#ifdef HAVE_BLUETOOTH
    native_data_t *nat = get_native_data(env, object);

    pthread_mutex_lock(&(nat->thread_mutex));
    if (nat->pollData) {
        char data = EVENT_LOOP_EXIT;
        ssize_t t = write(nat->controlFdW, &data, sizeof(char));
        void *ret;
        pthread_join(nat->thread, &ret);

        env->DeleteGlobalRef(nat->me);
        nat->me = NULL;
        free(nat->pollData);
        nat->pollData = NULL;
        free(nat->watchData);
        nat->watchData = NULL;
        nat->pollDataSize = 0;
        nat->pollMemberCount = 0;
        shutdown(nat->controlFdW, SHUT_RDWR);
        nat->controlFdW = 0;
        nat->controlFdR = 0;
    }
    pthread_mutex_unlock(&(nat->thread_mutex));
#endif // HAVE_BLUETOOTH
}

static jboolean isEventLoopRunningNative(JNIEnv *env, jobject object) {
    jboolean result = JNI_FALSE;
#ifdef HAVE_BLUETOOTH
    native_data_t *nat = get_native_data(env, object);

    pthread_mutex_lock(&(nat->thread_mutex));
    if (nat->pollData) {
        result = JNI_TRUE;
    }
    pthread_mutex_unlock(&(nat->thread_mutex));

#endif // HAVE_BLUETOOTH
    return result;
}

#ifdef HAVE_BLUETOOTH
extern DBusHandlerResult a2dp_event_filter(DBusMessage *msg, JNIEnv *env);

// Called by dbus during WaitForAndDispatchEventNative()
static DBusHandlerResult event_filter(DBusConnection *conn, DBusMessage *msg,
                                      void *data) {
    native_data_t *nat;
    JNIEnv *env;
    DBusError err;

    dbus_error_init(&err);

    nat = (native_data_t *)data;
    nat->vm->GetEnv((void**)&env, nat->envVer);
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) {
        LOGV("%s: not interested (not a signal).", __FUNCTION__);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    // STOPSHIP: Change to LOGV
    LOGE("%s: Received signal %s:%s from %s", __FUNCTION__,
        dbus_message_get_interface(msg), dbus_message_get_member(msg),
        dbus_message_get_path(msg));

    if (dbus_message_is_signal(msg,
                               "org.bluez.Adapter",
                               "DeviceFound")) {
        char *c_address;
        DBusMessageIter iter;
        jobjectArray str_array = NULL;
        if (dbus_message_iter_init(msg, &iter)) {
            dbus_message_iter_get_basic(&iter, &c_address);
            if (dbus_message_iter_next(&iter))
                str_array =
                    parse_remote_device_properties(env, &iter);
        }
        if (str_array != NULL) {
            env->CallVoidMethod(nat->me,
                                method_onDeviceFound,
                                env->NewStringUTF(c_address),
                                str_array);
        } else
            LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (dbus_message_is_signal(msg,
                                     "org.bluez.Adapter",
                                     "DeviceDisappeared")) {
        char *c_address;
        if (dbus_message_get_args(msg, &err,
                                  DBUS_TYPE_STRING, &c_address,
                                  DBUS_TYPE_INVALID)) {
            LOGV("... address = %s", c_address);
            env->CallVoidMethod(nat->me, method_onDeviceDisappeared,
                                env->NewStringUTF(c_address));
        } else LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (dbus_message_is_signal(msg,
                                     "org.bluez.Adapter",
                                     "DeviceCreated")) {
        char *c_object_path;
        if (dbus_message_get_args(msg, &err,
                                  DBUS_TYPE_OBJECT_PATH, &c_object_path,
                                  DBUS_TYPE_INVALID)) {
            LOGV("... address = %s", c_object_path);
            env->CallVoidMethod(nat->me,
                                method_onDeviceCreated,
                                env->NewStringUTF(c_object_path));
        } else LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (dbus_message_is_signal(msg,
                                     "org.bluez.Adapter",
                                     "DeviceRemoved")) {
        char *c_object_path;
        if (dbus_message_get_args(msg, &err,
                                 DBUS_TYPE_OBJECT_PATH, &c_object_path,
                                 DBUS_TYPE_INVALID)) {
           LOGV("... Object Path = %s", c_object_path);
           env->CallVoidMethod(nat->me,
                               method_onDeviceRemoved,
                               env->NewStringUTF(c_object_path));
        } else LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);

        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (dbus_message_is_signal(msg,
                                      "org.bluez.Adapter",
                                      "PropertyChanged")) {
        jobjectArray str_array = parse_adapter_property_change(env, msg);
        if (str_array != NULL) {
            /* Check if bluetoothd has (re)started, if so update the path. */
            jstring property =(jstring) env->GetObjectArrayElement(str_array, 0);
            const char *c_property = env->GetStringUTFChars(property, NULL);
            if (!strncmp(c_property, "Powered", strlen("Powered"))) {
                jstring value =
                    (jstring) env->GetObjectArrayElement(str_array, 1);
                const char *c_value = env->GetStringUTFChars(value, NULL);
                if (!strncmp(c_value, "true", strlen("true")))
                    nat->adapter = get_adapter_path(nat->conn);
                env->ReleaseStringUTFChars(value, c_value);
            }
            env->ReleaseStringUTFChars(property, c_property);

            env->CallVoidMethod(nat->me,
                              method_onPropertyChanged,
                              str_array);
        } else LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (dbus_message_is_signal(msg,
                                      "org.bluez.Device",
                                      "PropertyChanged")) {
        jobjectArray str_array = parse_remote_device_property_change(env, msg);
        if (str_array != NULL) {
            const char *remote_device_path = dbus_message_get_path(msg);
            env->CallVoidMethod(nat->me,
                            method_onDevicePropertyChanged,
                            env->NewStringUTF(remote_device_path),
                            str_array);
        } else LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        return DBUS_HANDLER_RESULT_HANDLED;

    }
    return a2dp_event_filter(msg, env);
}

// Called by dbus during WaitForAndDispatchEventNative()
DBusHandlerResult agent_event_filter(DBusConnection *conn,
                                     DBusMessage *msg, void *data) {
    native_data_t *nat = (native_data_t *)data;
    JNIEnv *env;
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
        LOGV("%s: not interested (not a method call).", __FUNCTION__);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    LOGI("%s: Received method %s:%s", __FUNCTION__,
         dbus_message_get_interface(msg), dbus_message_get_member(msg));

    if (nat == NULL) return DBUS_HANDLER_RESULT_HANDLED;

    nat->vm->GetEnv((void**)&env, nat->envVer);
    if (dbus_message_is_method_call(msg,
            "org.bluez.Agent", "Cancel")) {

        env->CallVoidMethod(nat->me, method_onAgentCancel);

        // reply
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) {
            LOGE("%s: Cannot create message reply\n", __FUNCTION__);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        dbus_connection_send(nat->conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;

    } else if (dbus_message_is_method_call(msg,
            "org.bluez.Agent", "Authorize")) {
        char *object_path;
        const char *uuid;
        if (!dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_OBJECT_PATH, &object_path,
                                   DBUS_TYPE_STRING, &uuid,
                                   DBUS_TYPE_INVALID)) {
            LOGE("%s: Invalid arguments for Authorize() method", __FUNCTION__);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }

        LOGV("... object_path = %s", object_path);
        LOGV("... uuid = %s", uuid);

        bool auth_granted =
            env->CallBooleanMethod(nat->me, method_onAgentAuthorize,
                env->NewStringUTF(object_path), env->NewStringUTF(uuid));

        // reply
        if (auth_granted) {
            DBusMessage *reply = dbus_message_new_method_return(msg);
            if (!reply) {
                LOGE("%s: Cannot create message reply\n", __FUNCTION__);
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }
            dbus_connection_send(nat->conn, reply, NULL);
            dbus_message_unref(reply);
        } else {
            DBusMessage *reply = dbus_message_new_error(msg,
                    "org.bluez.Error.Rejected", "Authorization rejected");
            if (!reply) {
                LOGE("%s: Cannot create message reply\n", __FUNCTION__);
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }
            dbus_connection_send(nat->conn, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (dbus_message_is_method_call(msg,
            "org.bluez.Agent", "RequestPinCode")) {
        char *object_path;
        if (!dbus_message_get_args(msg, NULL,
                                   DBUS_TYPE_OBJECT_PATH, &object_path,
                                   DBUS_TYPE_INVALID)) {
            LOGE("%s: Invalid arguments for RequestPinCode() method", __FUNCTION__);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }

        dbus_message_ref(msg);  // increment refcount because we pass to java
        env->CallVoidMethod(nat->me, method_onRequestPinCode,
                                       env->NewStringUTF(object_path),
                                       int(msg));
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (dbus_message_is_method_call(msg,
                  "org.bluez.Agent", "Release")) {
        // reply
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) {
            LOGE("%s: Cannot create message reply\n", __FUNCTION__);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        dbus_connection_send(nat->conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    } else {
        LOGV("%s:%s is ignored", dbus_message_get_interface(msg), dbus_message_get_member(msg));
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
#endif


#ifdef HAVE_BLUETOOTH
//TODO: Unify result codes in a header
#define BOND_RESULT_ERROR -1000
#define BOND_RESULT_SUCCESS 0
#define BOND_RESULT_AUTH_FAILED 1
#define BOND_RESULT_AUTH_REJECTED 2
#define BOND_RESULT_AUTH_CANCELED 3
#define BOND_RESULT_REMOTE_DEVICE_DOWN 4
#define BOND_RESULT_DISCOVERY_IN_PROGRESS 5

void onCreatePairedDeviceResult(DBusMessage *msg, void *user, void *n) {
    LOGV(__FUNCTION__);

    native_data_t *nat = (native_data_t *)n;
    const char *address = (const char *)user;
    DBusError err;
    dbus_error_init(&err);
    JNIEnv *env;
    nat->vm->GetEnv((void**)&env, nat->envVer);

    LOGV("... address = %s", address);

    jint result = BOND_RESULT_SUCCESS;
    if (dbus_set_error_from_message(&err, msg)) {
        if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.AuthenticationFailed")) {
            // Pins did not match, or remote device did not respond to pin
            // request in time
            LOGV("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_AUTH_FAILED;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.AuthenticationRejected")) {
            // We rejected pairing, or the remote side rejected pairing. This
            // happens if either side presses 'cancel' at the pairing dialog.
            LOGV("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_AUTH_REJECTED;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.AuthenticationCanceled")) {
            // Not sure if this happens
            LOGV("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_AUTH_CANCELED;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.ConnectionAttemptFailed")) {
            // Other device is not responding at all
            LOGV("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_REMOTE_DEVICE_DOWN;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.AlreadyExists")) {
            // already bonded
            LOGV("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_SUCCESS;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.InProgress") &&
                   !strcmp(err.message, "Bonding in progress")) {
            LOGV("... error = %s (%s)\n", err.name, err.message);
            goto done;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.InProgress") &&
                   !strcmp(err.message, "Discover in progress")) {
            LOGV("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_DISCOVERY_IN_PROGRESS;
        } else {
            LOGE("%s: D-Bus error: %s (%s)\n", __FUNCTION__, err.name, err.message);
            result = BOND_RESULT_ERROR;
        }
    }

    env->CallVoidMethod(nat->me,
                        method_onCreatePairedDeviceResult,
                        env->NewStringUTF(address),
                        result);
done:
    dbus_error_free(&err);
    free(user);
}

void onGetDeviceServiceChannelResult(DBusMessage *msg, void *user, void *n) {
    LOGV(__FUNCTION__);

    const char *address = (const char *) user;
    native_data_t *nat = (native_data_t *) n;

    DBusError err;
    dbus_error_init(&err);
    JNIEnv *env;
    nat->vm->GetEnv((void**)&env, nat->envVer);

    jint channel = -2;

    LOGV("... address = %s", address);

    if (dbus_set_error_from_message(&err, msg) ||
        !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_INT32, &channel,
                               DBUS_TYPE_INVALID)) {
        LOGE("%s: D-Bus error: %s (%s)\n", __FUNCTION__, err.name, err.message);
        dbus_error_free(&err);
    }

done:
    env->CallVoidMethod(nat->me,
                        method_onGetDeviceServiceChannelResult,
                        env->NewStringUTF(address),
                        channel);
    free(user);
}
#endif

static JNINativeMethod sMethods[] = {
     /* name, signature, funcPtr */
    {"classInitNative", "()V", (void *)classInitNative},
    {"initializeNativeDataNative", "()V", (void *)initializeNativeDataNative},
    {"cleanupNativeDataNative", "()V", (void *)cleanupNativeDataNative},
    {"startEventLoopNative", "()V", (void *)startEventLoopNative},
    {"stopEventLoopNative", "()V", (void *)stopEventLoopNative},
    {"isEventLoopRunningNative", "()Z", (void *)isEventLoopRunningNative}
};

int register_android_server_BluetoothEventLoop(JNIEnv *env) {
    return AndroidRuntime::registerNativeMethods(env,
            "android/server/BluetoothEventLoop", sMethods, NELEM(sMethods));
}

} /* namespace android */
