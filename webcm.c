#include <stdio.h>  
#include <stdlib.h>  
#include <string.h>  
#include <stdint.h>  
#include <time.h>  
  
#include "cartesi-machine/machine-c-api.h"  
#include <emscripten.h>  
  
#define MINIZ_NO_ARCHIVE_APIS  
#define MINIZ_NO_STDIO  
#define MINIZ_NO_TIME  
#define MINIZ_EXPORT static  
#include "third-party/miniz.h"  
#include "third-party/miniz.c"  
  
#define RAM_SIZE 128*1024*1024  
#define ROOTFS_SIZE 128*1024*1024  
#define RAM_START UINT64_C(0x80000000)  
#define ROOTFS_START UINT64_C(0x80000000000000)  
  
static uint8_t linux_bin_zz[] = {  
    #embed "linux.bin.zz"  
};  
  
static uint8_t rootfs_ext2_zz[] = {  
    #embed "rootfs.ext2.zz"  
};  
  
int hex_to_bin(const char *hex, uint8_t **bin, size_t *bin_len) {  
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {  
        hex += 2;
    }  
      
    size_t hex_len = strlen(hex);  
    *bin_len = hex_len / 2;  
    *bin = (uint8_t*)malloc(*bin_len);  
      
    if (!*bin) {  
        return 0;
    }  
      
    for (size_t i = 0; i < *bin_len; i++) {  
        sscanf(hex + i*2, "%2hhx", &(*bin)[i]);  
    }  
      
    return 1;  
}  
  
typedef struct uncompress_env {  
    cm_machine *machine;  
    uint64_t offset;  
} uncompress_env;  
  
int uncompress_cb(uint8_t *data, int size, uncompress_env *env) {  
    if (cm_write_memory(env->machine, env->offset, data, size) != CM_ERROR_OK) {  
        printf("failed to write machine memory: %s\n", cm_get_last_error_message());  
        exit(1);  
    }  
    env->offset += size;  
    return 1;  
}  
  
uint64_t uncompress_memory(cm_machine *machine, uint64_t paddr, uint8_t *data, uint64_t size) {  
    uncompress_env env = {machine, paddr};  
    size_t uncompressed_size = size;  
    if (tinfl_decompress_mem_to_callback(data, &uncompressed_size, (tinfl_put_buf_func_ptr)uncompress_cb, &env, TINFL_FLAG_PARSE_ZLIB_HEADER) != 1) {  
        printf("failed to uncompress memory\n");  
        exit(1);  
    }  
    return uncompressed_size;  
}  
  
void process_cmio_outputs(cm_machine *machine) {  
    cm_break_reason break_reason;  
      
    while (1) {  
        uint8_t cmd;  
        uint16_t reason;  
        uint8_t output_data[2*1024*1024]; // 2MB buffer for output  
        uint64_t length = sizeof(output_data);  
  
        if (cm_receive_cmio_request(machine, &cmd, &reason, output_data, &length) != CM_ERROR_OK) {  
            printf("Failed to receive CMIO request: %s\n", cm_get_last_error_message());  
            break;  
        }  
  
        if (cmd == CM_CMIO_YIELD_COMMAND_AUTOMATIC) {  
            if (reason == CM_CMIO_YIELD_AUTOMATIC_REASON_TX_OUTPUT) {  
                printf("Notice/Output received (%llu bytes):\n", (unsigned long long)length);  
                for (uint64_t i = 0; i < (length < 100 ? length : 100); i++) {  
                    printf("%02x", output_data[i]);  
                }  
                printf("\n");  
                  
                if (length > 0) {  
                    printf("As text: %.*s\n", (int)length, (char*)output_data);  
                }  
            } else if (reason == CM_CMIO_YIELD_AUTOMATIC_REASON_TX_REPORT) {  
                printf("Report received (%llu bytes):\n", (unsigned long long)length);  
                for (uint64_t i = 0; i < (length < 100 ? length : 100); i++) {  
                    printf("%02x", output_data[i]);  
                }  
                printf("\n");  
                  
                if (length > 0) {  
                    printf("As text: %.*s\n", (int)length, (char*)output_data);  
                }  
            } else if (reason == CM_CMIO_YIELD_AUTOMATIC_REASON_PROGRESS) {  
                uint32_t progress = *(uint32_t*)output_data;  
                printf("Progress: %f%%\n", progress / 10.0);  
            }  
        } else {  
            break;  
        }  
          
        if (cm_run(machine, CM_MCYCLE_MAX, &break_reason) != CM_ERROR_OK) {  
            printf("Failed to resume machine: %s\n", cm_get_last_error_message());  
            break;  
        }  
          
        if (break_reason != CM_BREAK_REASON_YIELDED_AUTOMATICALLY) {  
            break;  
        }  
          
        emscripten_sleep(0);  
    }  
}  
  
int send_hex_input(cm_machine *machine, const char *hex_input) {  
    uint8_t *binary_data;  
    size_t binary_length;  
      
    if (!hex_to_bin(hex_input, &binary_data, &binary_length)) {  
        printf("Failed to convert hex input to binary\n");  
        return 0;  
    }  
      
    int success = 1;  
    if (cm_send_cmio_response(machine, CM_CMIO_YIELD_REASON_ADVANCE_STATE, binary_data, binary_length) != CM_ERROR_OK) {  
        printf("Failed to send CMIO response: %s\n", cm_get_last_error_message());  
        success = 0;  
    }  
      
    free(binary_data);  
    return success;  
}  
  
int main() {  
    printf("Allocating...\n");  
  
    unsigned long long now = (unsigned long long)time(NULL);  
    char config[4096];  
    snprintf(config, sizeof(config), "{\  
        \"dtb\": {\  
            \"bootargs\": \"quiet earlycon=sbi console=hvc1 root=/dev/pmem0 rw init=/usr/sbin/cartesi-init\",\  
            \"init\": \"date -s @%llu >> /dev/null\",\  
            \"entrypoint\": \"export ROLLUP_HTTP_SERVER_URL=http://localhost:5004 && rollup-init python dapp.py\"\  
        },\  
        \"flash_drive\": [\  
            {\"length\": %u}\  
        ],\  
        \"virtio\": [\  
            {\"type\": \"console\"}\  
        ],\  
        \"htif\": {\  
            \"yield_automatic\": true,\  
            \"yield_manual\": true\  
        },\  
        \"cmio\": {\  
            \"rx_buffer\": {\"shared\": false},\  
            \"tx_buffer\": {\"shared\": false}\  
        },\  
        \"processor\": {\  
            \"iunrep\": 1\  
        },\  
        \"ram\": {\"length\": %u}\  
    }", now, RAM_SIZE, ROOTFS_SIZE);  
  
    // Create a new machine  
    cm_machine *machine = NULL;  
    if (cm_create_new(config, NULL, &machine) != CM_ERROR_OK) {  
        printf("failed to create machine: %s\n", cm_get_last_error_message());  
        exit(1);  
    }  
  
    printf("Decompressing...\n");  
  
    // Decompress kernel and rootfs  
    uncompress_memory(machine, RAM_START, linux_bin_zz, sizeof(linux_bin_zz));  
    uncompress_memory(machine, ROOTFS_START, rootfs_ext2_zz, sizeof(rootfs_ext2_zz));  
  
    printf("Booting...\n");  
  
    // Run the machine  
    cm_break_reason break_reason;  
    do {  
        uint64_t mcycle;  
        if (cm_read_reg(machine, CM_REG_MCYCLE, &mcycle) != CM_ERROR_OK) {  
            printf("failed to read machine cycle: %s\n", cm_get_last_error_message());  
            cm_delete(machine);  
            exit(1);  
        }  
        if (cm_run(machine, mcycle + 4*1024*1024, &break_reason) != CM_ERROR_OK) {  
            printf("failed to run machine: %s\n", cm_get_last_error_message());  
            cm_delete(machine);  
            exit(1);  
        }  
        emscripten_sleep(0);  
    } while(break_reason == CM_BREAK_REASON_REACHED_TARGET_MCYCLE);  
  
    printf("Initial boot completed with reason: ");  
    switch (break_reason) {  
        case CM_BREAK_REASON_HALTED:  
            printf("Halted\n");  
            break;  
        case CM_BREAK_REASON_YIELDED_MANUALLY:  
            printf("Yielded manually\n");  
            // Machine is waiting for input, send our hex input  
            if (break_reason == CM_BREAK_REASON_YIELDED_MANUALLY) {  
                const char *hex_input = "0x415bf363000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000020000000000000000000000000000000000000000000000000000000000000003000000000000000000000000000000000000000000000000000000000000000400000000000000000000000000000000000000000000000000000000000000050000000000000000000000000000000000000000000000000000000000000006000000000000000000000000000000000000000000000000000000000000000700000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000009616476616e63652d300000000000000000000000000000000000000000000000";  
                  
                uint8_t cmd;  
                uint16_t reason;  
                uint8_t request_data[1024];  
                uint64_t length = sizeof(request_data);  
                  
                if (cm_receive_cmio_request(machine, &cmd, &reason, request_data, &length) == CM_ERROR_OK) {  
                    printf("Received request, sending hex input...\n");  
                      


                    if (send_hex_input(machine, hex_input)) {  
                        printf("Hex input sent successfully\n");  
                          
                        if (cm_run(machine, CM_MCYCLE_MAX, &break_reason) == CM_ERROR_OK) {  
                            process_cmio_outputs(machine);  
                        }  
                    }  
                }  
            }  
            break;  
        case CM_BREAK_REASON_YIELDED_AUTOMATICALLY:  
            printf("Yielded automatically\n");  
            // Process any outputs  
            process_cmio_outputs(machine);  
            break;  
        case CM_BREAK_REASON_YIELDED_SOFTLY:  
            printf("Yielded softly\n");  
            break;  
        case CM_BREAK_REASON_REACHED_TARGET_MCYCLE:  
            printf("Reached target machine cycle\n");  
            break;  
        case CM_BREAK_REASON_FAILED:  
        default:  
            printf("Interpreter failed\n");  
            break;  
    }  
  
    // Read and print machine cycles  
    uint64_t mcycle;  
    if (cm_read_reg(machine, CM_REG_MCYCLE, &mcycle) != CM_ERROR_OK) {  
        printf("failed to read machine cycle: %s\n", cm_get_last_error_message());  
        cm_delete(machine);  
        exit(1);  
    }  
    printf("Cycles: %lu\n", (unsigned long)mcycle);  
  
    // Cleanup and exit  
    cm_delete(machine);  
    return 0;  
}