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
  
// Global variables for machine control  
static cm_machine *g_machine = NULL;  
static int machine_running = 0;  
static int machine_initialized = 0;  
  
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
  
// Function to process outputs and send to JavaScript  
void process_outputs() {  
    uint8_t cmd;  
    uint16_t reason;  
    uint8_t *output_data = malloc(64*1024); // 64KB should be sufficient  
    uint64_t length = 64*1024;  
      
    if (!output_data) {  
        printf("Failed to allocate output buffer\n");  
        return;  
    }  
      
    if (cm_receive_cmio_request(g_machine, &cmd, &reason, output_data, &length) == CM_ERROR_OK) {  
        if (cmd == CM_CMIO_YIELD_COMMAND_AUTOMATIC) {  
            // Send output to JavaScript using EM_ASM  
            EM_ASM({  
                if (window.onMachineOutput) {  
                    const data = new Uint8Array(Module.HEAPU8.buffer, Number($1), Number($2));  
                    const text = new TextDecoder().decode(data);  
                    window.onMachineOutput($0, text, data);  
                }  
            }, reason, output_data, length);  
        }  
    }  
      
    free(output_data);  
}  
  
// Function to initialize the machine (called once)  
EMSCRIPTEN_KEEPALIVE  
int init_machine() {  
    if (machine_initialized) {  
        return 1; // Already initialized  
    }  
      
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
    }", now, ROOTFS_SIZE, RAM_SIZE);  
      
    // Create a new machine  
    if (cm_create_new(config, NULL, &g_machine) != CM_ERROR_OK) {  
        printf("failed to create machine: %s\n", cm_get_last_error_message());  
        return -1;  
    }  
      
    printf("Decompressing...\n");  
      
    // Decompress kernel and rootfs  
    uncompress_memory(g_machine, RAM_START, linux_bin_zz, sizeof(linux_bin_zz));  
    uncompress_memory(g_machine, ROOTFS_START, rootfs_ext2_zz, sizeof(rootfs_ext2_zz));  
      
    printf("Booting...\n");  
      
    // Initial machine boot  
    cm_break_reason break_reason;  
    do {  
        uint64_t mcycle;  
        if (cm_read_reg(g_machine, CM_REG_MCYCLE, &mcycle) != CM_ERROR_OK) {  
            printf("failed to read machine cycle: %s\n", cm_get_last_error_message());  
            cm_delete(g_machine);  
            g_machine = NULL;  
            return -2;  
        }  
        if (cm_run(g_machine, mcycle + 4*1024*1024, &break_reason) != CM_ERROR_OK) {  
            printf("failed to run machine: %s\n", cm_get_last_error_message());  
            cm_delete(g_machine);  
            g_machine = NULL;  
            return -3;  
        }  
        emscripten_sleep(0);  
    } while(break_reason == CM_BREAK_REASON_REACHED_TARGET_MCYCLE);  
      
    printf("Initial boot completed with reason: %d\n", break_reason);  
      
    machine_running = 1;  
    machine_initialized = 1;  
    return 0;  
}  
  
// Function to send input (called from JavaScript)  
EMSCRIPTEN_KEEPALIVE  
int send_input(const char *hex_input) {  
    if (!g_machine || !machine_running) {  
        return -1;  
    }  
      
    uint8_t *binary_data;  
    size_t binary_length;  
      
    if (!hex_to_bin(hex_input, &binary_data, &binary_length)) {  
        printf("Failed to convert hex input to binary\n");  
        return -2;  
    }  
      
    if (cm_send_cmio_response(g_machine, CM_CMIO_YIELD_REASON_ADVANCE_STATE,   
                              binary_data, binary_length) != CM_ERROR_OK) {  
        printf("Failed to send CMIO response: %s\n", cm_get_last_error_message());  
        free(binary_data);  
        return -3;  
    }  
      
    free(binary_data);  
    return 0;  
}  
  
// Function to process one cycle (called repeatedly)  
EMSCRIPTEN_KEEPALIVE  
int process_cycle() {  
    if (!g_machine || !machine_running) {  
        return -1;  
    }  
      
    cm_break_reason break_reason;  
    uint64_t current_cycle;  
      
    if (cm_read_reg(g_machine, CM_REG_MCYCLE, &current_cycle) != CM_ERROR_OK) {  
        printf("Failed to read mcycle: %s\n", cm_get_last_error_message());  
        return -2;  
    }  
      
    // Execute for limited cycles to not freeze the browser  
    if (cm_run(g_machine, current_cycle + 4*1024*1024, &break_reason) != CM_ERROR_OK) {  
        printf("Failed to run machine: %s\n", cm_get_last_error_message());  
        machine_running = 0; // Mark as stopped on error  
        return -3;  
    }  
      
    switch (break_reason) {  
        case CM_BREAK_REASON_YIELDED_AUTOMATICALLY:  
            // Process outputs and continue running  
            process_outputs();  
            return 1; // Continue processing to reach manual yield  
              
        case CM_BREAK_REASON_YIELDED_MANUALLY:  
            return 2; // Ready for next input  
              
        case CM_BREAK_REASON_HALTED:  
            machine_running = 0;  
            return 0; // Machine stopped  
              
        case CM_BREAK_REASON_REACHED_TARGET_MCYCLE:  
            return 1; // Continue processing  
              
        default:  
            printf("Unexpected break reason: %d\n", break_reason);  
            machine_running = 0;  
            return -4; // Error  
    }  
}  
  
// Function to destroy the machine  
EMSCRIPTEN_KEEPALIVE  
void destroy_machine() {  
    if (g_machine) {  
        cm_delete(g_machine);  
        g_machine = NULL;  
        machine_running = 0;  
        machine_initialized = 0;  
    }  
}  
  
// Function to check machine status  
EMSCRIPTEN_KEEPALIVE  
int get_machine_status() {  
    if (!machine_initialized) return 0; // Not initialized  
    if (!machine_running) return 1; // Initialized but stopped  
    return 2; // Running  
}  
  
// Original main function kept for compatibility  
int main() {  
    // Initialization is now done via init_machine()  
    printf("WebCM ready. Call init_machine() to start.\n");  
    return 0;  
}