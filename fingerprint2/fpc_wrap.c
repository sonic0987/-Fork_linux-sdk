#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <utils/log.h>

#include <utils/assert.h>
#include <utils/common.h>
#include <utils/runnable/default_runnable.h>
#include <gpio/gpio_manager.h>
#include "fpc_wrap.h"



#define LOG_TAG                         "fpc_wrap"

#define FP_DEVICE_NAME                  "/dev/fpcdev0"
#define UART_DEVICE_NAME                "/dev/ttyS1"


#define FPC_FP_DATA_LIST_NAME           "fpc_fp_data_list"
#define FPC_FP_ID_LIST_NAME             "fpc_fp_id_list"

#define FPC_IOC_MAGIC                   'F'
#define READ_IMAGE                      _IOR(FPC_IOC_MAGIC, 0, int)
#define HW_RESET                        _IOW(FPC_IOC_MAGIC, 1, int)
#define ENCRYPT_IC_RST                  _IOW(FPC_IOC_MAGIC, 2, int)

#define DELETE_TYPE_SINGLE              0
#define DELETE_TYPE_ALL                 1

#define ENABLE_DUP_CHECK                1
#define DISABLE_DUP_CHECK               0

#define IMAGE_SIZE                      (242*266)

static struct default_runnable* thread_runner;
static notify_callback callback;
static customer_config_t config;
static Init_infor init_info;
static int fpc_dev_fd;
static int local_pipe[2];

static uint8_t* img_data = NULL;
static uint32_t* id_list = NULL;
static int id_list_fd = -1;
static uint32_t template_num = 0;

static pthread_mutex_t id_list_mutex = PTHREAD_MUTEX_INITIALIZER;


enum {
    MSG_QUIT = 0,
    MSG_ENROLL,
    MSG_AUTHENTICATE,
    MSG_DELETE,
};

struct pipo_msg {
    uint8_t  msg;
    uint32_t param[3];
};


static const char* msg2str(uint8_t msg)
{
    switch(msg) {
    case MSG_QUIT:
        return "quit";

    case MSG_ENROLL:
        return "enroll";

    case MSG_AUTHENTICATE:
        return "authenticate";

    case MSG_DELETE:
        return "delete";

    default:
        return "unknown";
    }
}

static int send_msg(struct pipo_msg* pp_msg)
{
    int error = 0;

    pthread_mutex_lock(&id_list_mutex);
    error = write(local_pipe[1], pp_msg, sizeof(struct pipo_msg));
    if (error < 0) {
        LOGE("Failed to send \'%s\' %s\n", msg2str(pp_msg->msg),strerror(errno));
        pthread_mutex_unlock(&id_list_mutex);
        return -1;
    }

    pthread_mutex_unlock(&id_list_mutex);
    return 0;
}


static int read_id_list_items(uint32_t num, uint32_t items[])
{
    int i, ret;

    pthread_mutex_lock(&id_list_mutex);
    ret = lseek(id_list_fd, 0, SEEK_SET);
    if (ret < 0) {
        LOGE("Failed to file lseek. ret: %d %s\n",ret, strerror(errno));
        pthread_mutex_unlock(&id_list_mutex);
        return -1;
    }

    for (i = 0; i < num; ++i) {
        ret = read(id_list_fd, &items[i], sizeof(uint32_t));
        if (ret < 0) {
            LOGE("Failed to read list id. ret: %x %s\n",ret, strerror(errno));
            pthread_mutex_unlock(&id_list_mutex);
            return -1;
        }
        if (ret == 0) {
            LOGE("Read list id finish. ret: %x %s\n",ret, strerror(errno));
            break;
        }
    }
    pthread_mutex_unlock(&id_list_mutex);
    return 0;
}


static int write_id_list_tail(uint32_t id)
{
    int ret;

    pthread_mutex_lock(&id_list_mutex);
    ret = write(id_list_fd, &id, sizeof(id));
    if (ret != sizeof(id)) {
        LOGE("Failed to write id to template id list. ret: %d %s\n",ret, strerror(errno));
        pthread_mutex_unlock(&id_list_mutex);
        return -1;
    }

    pthread_mutex_unlock(&id_list_mutex);
    return 0;
}

static int remove_id_list_item(uint32_t id)
{
    int i,ret;
    uint32_t id_num;
    uint32_t cur_pos;

    pthread_mutex_lock(&id_list_mutex);
    cur_pos = lseek(id_list_fd, 0, SEEK_CUR);
    ret = lseek(id_list_fd, 0, SEEK_SET);
    if (ret < 0) {
        LOGE("Failed to file lseek. ret: %d %s\n",ret, strerror(errno));
        goto file_operate_fail;
    }
    ret = read(id_list_fd, id_list, template_num*sizeof(uint32_t));
    if (ret < 0) {
        LOGE("Failed to read file. ret: %d %s\n",id, strerror(errno));
        goto file_operate_fail;
    }

    id_num = ret/4;
    for (i = 0; i < id_num; ++i) {
        if (id_list[i] == id) {
           id_list[i] = -1;
           break;
        }
    }
    if (i != id_num) {
        ret = lseek(id_list_fd, 0, SEEK_SET);
        if (ret < 0) {
            LOGE("Failed to file lseek. ret: %d %s\n",id, strerror(errno));
            goto file_operate_fail;
        }
        for (i = 0; i < id_num; ++i) {
            if (id_list[i] == -1) {
                continue;
            }
            ret = write(id_list_fd, &id_list[i], sizeof(id));
            if (ret < 0) {
                LOGE("Failed to write file. ret: %d %s\n",id, strerror(errno));
            }
        }
    } else {
        LOGE("Can not found id: %d \n", id);
        goto file_operate_fail;
    }

    pthread_mutex_unlock(&id_list_mutex);
    return 0;

file_operate_fail:
    lseek(id_list_fd, 0, SEEK_SET);
    lseek(id_list_fd, cur_pos, SEEK_SET);
    pthread_mutex_unlock(&id_list_mutex);
    return -1;
}

static int reset_id_list()
{
    int ret;

    pthread_mutex_lock(&id_list_mutex);
    ret = lseek(id_list_fd, 0, SEEK_SET);
    if (ret < 0) {
        LOGE("Failed to file lseek. ret: %d %s\n",ret, strerror(errno));
        pthread_mutex_unlock(&id_list_mutex);
        return -1;
    }

    pthread_mutex_unlock(&id_list_mutex);
    return 0;

}

static int enroll_msg_handle(uint32_t* id)
{
    int ret;
    uint32_t dup_id;

    ret = FpCommand(FF_GET_EMPTY_ID_CODE,(FULL_UINT)id,0,0);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to quit thread runner. ret: %x\n",ret);
        goto out;
    }

    ret = FpCommand(FF_ENROLL_CODE,0,(FULL_UINT)(*id),(FULL_UINT)&dup_id);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to quit thread runner. ret: %x\n",ret);
        goto out;
    }

    ret = FpCommand(FF_GET_IMAGE_CODE,(FULL_UINT)img_data,0,0);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to scapture image in step 1. ret: %x\n",ret);
        goto out;
    }

    ret = FpCommand(FF_ENROLL_CODE,1,(FULL_UINT)(*id),(FULL_UINT)&dup_id);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to enroll finger in step 1. ret: %x\n",ret);
        goto out;
    }
    callback(FPC_ENROLL_ING, 33, (*id));

    ret = FpCommand(FF_GET_IMAGE_CODE,(FULL_UINT)img_data,0,0);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to scapture image in step 2. ret: %x\n",ret);
        goto out;
    }

    ret = FpCommand(FF_ENROLL_CODE,2,(FULL_UINT)(*id),(FULL_UINT)&dup_id);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to enroll finger in step 2. ret: %x\n",ret);
        goto out;
    }
    callback(FPC_ENROLL_ING, 66, (*id));

    ret = FpCommand(FF_GET_IMAGE_CODE,(FULL_UINT)img_data,0,0);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to scapture image in step 3. ret: %x\n",ret);
        goto out;
    }

    ret = FpCommand(FF_ENROLL_CODE,3,(FULL_UINT)(*id),(FULL_UINT)&dup_id);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to enroll finger in step 3. ret: %x\n",ret);
        goto out;
    }
    callback(FPC_ENROLL_ING, 99, (*id));

    dup_id = 3;
    ret = FpCommand(FF_ENROLL_CODE,4,(FULL_UINT)(*id),(FULL_UINT)&dup_id);
    if (ret != FF_SUCCESS) {
        *id = dup_id;
        LOGE("Failed to enroll finger in step 4. ret: %x\n",ret);
        goto out;
    }

    ret = write_id_list_tail(*id);
    if (ret < 0) {
        ret = FpCommand(FF_CLEAR_TEMPLATE_CODE,(FULL_UINT)(*id),1,0);
        if (ret != FF_SUCCESS) {
            LOGE("Failed to delete fingerprint id: %d. ret: %x\n", *id, ret);
        }
        ret = FF_FAIL;
        goto out;
    }

    template_num++;
    ret = FF_SUCCESS;
out:
    return ret;
}


static int authenticate_msg_handle(uint32_t* id)
{
    int ret;
    uint32_t learn_buff;
    uint32_t auto_learn;

    ret = FpCommand(FF_GET_IMAGE_CODE,(FULL_UINT)img_data,0,0);
    if(ret != FF_SUCCESS){
        LOGE("Failed to capture authenticate of image. ret: %x\n",ret);
        goto out;
    }

    ret = FpCommand(FF_IDENTIFY_CODE,(FULL_UINT)id,(FULL_UINT)&learn_buff,0);
    if(ret != FF_SUCCESS){
        LOGE("Failed to authenticate. ret: %x\n",ret);
        goto out;
    }

    ret = FpCommand(FF_UPDATE_TMPL_CODE,(FULL_UINT)&auto_learn,0,0);
    if(ret != FF_SUCCESS && auto_learn != 1) {
        LOGE("Failed to update template. result: %d ret: %d\n",auto_learn, ret);
    }

    ret = FF_SUCCESS;
out:
    return ret;
}

static int delete_msg_handle(uint32_t id, uint32_t type)
{
    int ret;

    if (type == DELETE_TYPE_SINGLE) {
        ret = FpCommand(FF_CLEAR_TEMPLATE_CODE,(FULL_UINT)id,1,0);
        if (ret != FF_SUCCESS) {
            LOGE("Failed to delete fingerprint id: %d. ret: %x\n",id, ret);
            goto out;
        }
        ret = remove_id_list_item(id);
        if (ret < 0) {
            LOGE("Failed to remove id[%d]", id);
            goto out;
        }
        template_num--;
    } else if (type == DELETE_TYPE_ALL) {
        ret = FpCommand(FF_CLEAR_ALLTEMPLATE_CODE,0,0,0);
        if (ret != FF_SUCCESS) {
            LOGE("Failed to delete all fingerprint. ret: %x\n", ret);
            goto out;
        }
        ret = reset_id_list();
        if (ret < 0) {
            LOGE("Failed to reset id list file. ret: %d \n",ret);
            goto out;
        }
        template_num = 0;
    }

    ret = FF_SUCCESS;
out:
    return ret;
}

static int handle_msg(struct pipo_msg pp_msg)
{
    uint32_t tmpl_id;
    int ret;

    switch(pp_msg.msg)
    {
        case MSG_QUIT:
            return 1;

        case MSG_ENROLL:
            ret = enroll_msg_handle(&tmpl_id);
            if (ret == FF_SUCCESS) {
                callback(FPC_ENROLL_SUCCESS, 100, tmpl_id);
            } else if (ret == FF_ERR_DUPLICATION_ID) {
                callback(FPC_ENROLL_DUPLICATE, 100, tmpl_id);
            } else {
                callback(FPC_ENROLL_FAILED, 0, 0);
            }
            break;

        case MSG_AUTHENTICATE:
            ret = authenticate_msg_handle(&tmpl_id);
            if (ret == FF_SUCCESS) {
                callback(FPC_AUTHENTICATE_SUCCESS, 0, tmpl_id);
            } else {
                callback(FPC_AUTHENTICATED_FAILED, 0, 0);
            }
            break;

        case MSG_DELETE:
            ret = delete_msg_handle(pp_msg.param[0], pp_msg.param[1]);
            if (ret == FF_SUCCESS) {
                callback(FPC_REMOVE_SUCCESS, 0, 0);
            } else {
                callback(FPC_REMOVE_FAILED, 0, 0);
            }
            break;

        default:
            break;
    }

    return 0;
}

static void thread_loop(struct pthread_wrapper* thread, void* param) {
    int count;
    int error;
    struct pipo_msg pp_msg = {0};

    struct pollfd fds[1];

    fds[0].fd = local_pipe[0];
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    for (;;) {
        do {
            count = poll(fds, 1, -1);
        } while (count < 0 && errno == EINTR);

        if (fds[0].revents & POLLIN) {
            fds[0].revents = 0;
            error = read(fds[0].fd, &pp_msg, sizeof(struct pipo_msg));
            if (!error) {
                LOGE("Unable to read pipe: %s\n", strerror(errno));
                continue;
            }

            if (handle_msg(pp_msg) > 0) {
                LOGI("main thread call me break out\n");
                break;
            }
        }
    }
}

int fpc_fingerprint_init(notify_callback notify, void *param_config)
{
    assert_die_if(notify == NULL, "notify is NULL\n");
    assert_die_if(notify == NULL, "param_config is NULL\n");

    uint8_t filename[128];
    int error = 0;

    callback = notify;
    memcpy(&config, param_config, sizeof(customer_config_t));
    fpc_dev_fd = open(FP_DEVICE_NAME, O_RDWR);
    if (fpc_dev_fd < 0) {
        LOGE("Faild to open %s . err: %d\n", FP_DEVICE_NAME, fpc_dev_fd);
        goto err_fpc_dev_open;
    }

    init_info.Sensor_Dev = fpc_dev_fd;
    sprintf(init_info.Encrypt_Dev,UART_DEVICE_NAME);
    sprintf(init_info.Sys_filename,"%s/%s",get_user_system_dir(getuid()),
                                                FPC_FP_DATA_LIST_NAME);

    error = FpCommand(FF_INITIALIZE_CODE,(FULL_UINT)&init_info,0,0);
    if (error != FF_SUCCESS) {
        LOGE("Failed to command FF_INITIALIZE_CODE : %d\n", error);
        goto err_fp_cmd_init;
    }

    error = FpCommand(FF_SET_SECURITYLEVEL_CODE,3,0,0);
    if (error != FF_SUCCESS) {
        LOGE("Failed to command FF_SET_SECURITYLEVEL_CODE : %d\n", error);
        goto err_fp_cmd_set;
    }

    error = FpCommand(FF_SET_DUP_CHECK_CODE,DISABLE_DUP_CHECK,0,0);
    if (error != FF_SUCCESS) {
        LOGE("Failed to command FF_SET_DUP_CHECK_CODE : %d\n", error);
        goto err_fp_cmd_set;
    }

    error = FpCommand(FF_GET_ENROLL_COUNT_CODE,(FULL_UINT)&template_num,0,0);
    if (error != FF_SUCCESS) {
        LOGE("Failed to require enrolled count. ret: %x\n",error);
        goto err_fp_cmd_set;
    }

    error = pipe(local_pipe);
    if (error) {
        LOGE("Unable to open pipe: %s\n", strerror(errno));
        goto err_pipe;
    }

    thread_runner = _new(struct default_runnable, default_runnable);
    if (thread_runner == NULL) {
        LOGE("Failed to new thread\n");
        goto err_thread_new;
    }
    thread_runner->runnable.run = thread_loop;
    error = thread_runner->start(thread_runner, NULL);
    if (error < 0) {
        LOGE("Failed to start thread\n");
        goto err_thread_start;
    }
    if (img_data == NULL) {
        img_data = (uint8_t*)malloc(IMAGE_SIZE);
        if (img_data == NULL) {
            LOGE("Failed to malloc img_data\n");
            goto err_malloc_img_data;
        }
    } else {
        LOGE("Img_data isn't NUll\n");
        goto err_malloc_img_data;
    }

    if (id_list == NULL) {
        id_list = (uint32_t*)malloc(config.max_enroll_finger_num*4);
        if (id_list == NULL) {
            LOGE("Failed to malloc id list buffer\n");
            goto err_malloc_id_list;
        }
    }

    sprintf((char*)filename, "%s/%s", config.file_path, FPC_FP_ID_LIST_NAME);
    id_list_fd = open((char*)filename, O_RDWR|O_CREAT|O_SYNC, 0644);
    if (id_list_fd < 0) {
        LOGE("Failed to open %s.\n",filename);
        goto err_file_open;
    }
    return 0;

err_file_open:
    free(id_list);
err_malloc_id_list:
    free(img_data);
err_malloc_img_data:
    thread_runner->stop(thread_runner);
err_thread_start:
    _delete(thread_runner);
err_thread_new:
    if (local_pipe[0] > 0)
        close(local_pipe[0]);
    if (local_pipe[1] > 0)
        close(local_pipe[1]);
err_pipe:
err_fp_cmd_set:
    FpCommand(FF_TERMINATE,0,0,0);
err_fp_cmd_init:
    close(fpc_dev_fd);
err_fpc_dev_open:
    return -1;
}

int fpc_fingerprint_destroy(void)
{
    struct pipo_msg pp_msg = {0};

    pp_msg.msg = MSG_QUIT;
    if (send_msg(&pp_msg)) {
        LOGE("Failed to quit thread runner\n");
        return -1;
    }

    thread_runner->wait(thread_runner);
    _delete(thread_runner);

    if (local_pipe[0] > 0)
        close(local_pipe[0]);

    if (local_pipe[1] > 0)
        close(local_pipe[1]);

    free(img_data);

    FpCommand(FF_TERMINATE,0,0,0);
    close(fpc_dev_fd);
    return 0;
}

int fpc_fingerprint_reset(void)
{
    int ret = 0;

    ret = ioctl(fpc_dev_fd, HW_RESET, 0);
    if (ret < 0) {
        LOGE("HW_RESET ioctl fail. err: %d\n", ret);
        return -1;
    }

    return 0;
}

int fpc_fingerprint_enroll(void)
{
    int ret;
    uint32_t enrolled_count = 0;
    struct pipo_msg pp_msg = {0};

    ret = FpCommand(FF_GET_ENROLL_COUNT_CODE,(FULL_UINT)&enrolled_count,0,0);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to require enrolled count. ret: %x\n",ret);
        return -1;
    }

    if (enrolled_count >= config.max_enroll_finger_num) {
        LOGE("Failed to enroll, list is full\n");
        return -1;
    }

    pp_msg.msg = MSG_ENROLL;
    if (send_msg(&pp_msg)) {
        LOGE("Failed to send enroll msg.\n");
        return -1;
    }

    return 0;
}

int fpc_fingerprint_authenticate(void)
{
    struct pipo_msg pp_msg = {0};

    pp_msg.msg = MSG_AUTHENTICATE;
    if (send_msg(&pp_msg)) {
        LOGE("Failed to send authenticate msg.\n");
        return -1;
    }

    return 0;
}

int fpc_fingerprint_delete(int fingerprint_id, int type)
{
    struct pipo_msg pp_msg = {0};

    pp_msg.msg = MSG_DELETE;
    pp_msg.param[0] = fingerprint_id;
    pp_msg.param[1] = type;

    if (send_msg(&pp_msg)) {
        LOGE("Failed to send delete msg.\n");
        return -1;
    }
    return 0;
}

int fpc_fingerprint_cancel(void) {
    return -1;
}

int fpc_fingerprint_get_template_info(uint32_t template_info[])
{
    int ret;
    uint32_t enrolled_count = 0;

    ret = FpCommand(FF_GET_ENROLL_COUNT_CODE,(FULL_UINT)&enrolled_count,0,0);
    if (ret != FF_SUCCESS) {
        LOGE("Failed to require enrolled count. ret: %x\n",ret);
        return -1;
    }

    if (enrolled_count == 0) {
        LOGE("The template library is empty\n");
        return 0;
    }

    ret = read_id_list_items(enrolled_count, template_info);
    if (ret < 0) {
        LOGE("Failed to read id list items.");
        return -1;
    }

    return enrolled_count;
}
