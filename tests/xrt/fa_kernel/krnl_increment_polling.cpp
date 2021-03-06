#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <memory>
#include <chrono>
#include <sys/mman.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>

#include "xrt.h"
#include "ert.h"
#include "xclbin.h"
#include "experimental/xrt-next.h"

namespace fa {
    typedef enum {
        DESC_FIFO_OVERRUN = 0x1,
        DESC_DECERR       = 0x2,
        TASKCOUNT_DECERR  = 0x4,
    } error_t;

    typedef enum {
        UNDEFINED = 0xFFFFFFFF,
        ISSUED    = 0x0,
        COMPLETED = 0x1,
    } status_t;

    typedef struct {
        uint32_t     argOffset;   // offset within the acc aperture
        uint32_t     argSize;     // size of argument in bytes
        //void        *argValue;    // value of argument
        uint32_t     argValue[];
    } descEntry_t;

    typedef struct {
        uint32_t     status;            // descriptor control synchronization word
        uint32_t     numInputEntries;   // number of input arg entries
        uint32_t     inputEntryBytes;   // total number of bytes for input args
        uint32_t     numOutputEntries;  // number of output arg entries
        uint32_t     outputEntryBytes;  // total number of bytes for output args
        //descEntry_t *inputEntries;      // array of input entries
        //descEntry_t *outputEntries;     // array of output entries
        uint32_t     data[];
    } descriptor_t;

    void print_descriptor(descriptor_t *desc) {
        int i, j;
        int off;
        descEntry_t *entry;

        std::cout << "status            0x" << std::hex << desc->status << "\n";
        std::cout << "numInputEntries   0x" << desc->numInputEntries << "\n";
        std::cout << "inputEntryBytes   0x" << desc->inputEntryBytes << "\n";
        std::cout << "numOutputEntries  0x" << desc->numOutputEntries << "\n";
        std::cout << "outputEntryBytes  0x" << desc->outputEntryBytes << "\n";
        off = 0;
        for (i = 0; i < desc->numInputEntries; i++) {
            entry = reinterpret_cast<descEntry_t *>(desc->data + off);
            std::cout << "input descEntry (" << i << ")\n";
            std::cout << "    argOffset  0x" << entry->argOffset << "\n";
            std::cout << "    argSize    0x" << entry->argSize << "\n";
            for (j = 0; j < entry->argSize/4; j++) {
                std::cout << "    argValue   0x" << entry->argValue[j] << "\n";
            }
            off += (sizeof(descEntry_t) + entry->argSize)/4;
        }

        for (i = 0; i < desc->numOutputEntries; i++) {
            entry = reinterpret_cast<descEntry_t *>(desc->data + off);
            std::cout << "output descEntry(" << i << ")\n";
            std::cout << "    argOffset  0x" << entry->argOffset << "\n";
            std::cout << "    argSize    0x" << entry->argSize << "\n";
            for (j = 0; j < entry->argSize/4; j++) {
                std::cout << "    argValue   0x" << entry->argValue[j] << "\n";
            }
            off += (sizeof(descEntry_t) + entry->argSize)/4;
        }
        std::cout << std::dec;
    }
};

/* The Increment kernel needs 3 arguments
 * 0x10 mem, size 8 bytes
 */

int get_input_entries_size()
{
    int size;

    /* Add entry size of mem */
    size = sizeof(fa::descEntry_t) + 8;

    return size;
}

int get_output_entries_size()
{
    return 0;
}

int get_desc_size()
{
    int size;

    /* Descriptor - sizeof(data) */
    size = sizeof(fa::descriptor_t);

    size += get_input_entries_size();

    size += get_output_entries_size();

    return size;
}

#define DESC_FIFO_DEPTH 12

struct task_info {
    unsigned                buf_boh;
    unsigned                desc_bo;
    unsigned                exec_bo;
    uint64_t                desc_paddr;
    fa::descriptor_t       *desc;
    ert_start_kernel_cmd   *ecmd;
};

void usage()
{
    printf("Usage: test -k <xclbin>\n");
}

static std::vector<char>
load_file_to_memory(const std::string& fn)
{
    if (fn.empty())
        throw std::runtime_error("No xclbin specified");

    // load bit stream
    std::ifstream stream(fn);
    if (!stream.good())
        throw std::runtime_error("xclbin path is incorrect");
    stream.seekg(0,stream.end);
    size_t size = stream.tellg();
    stream.seekg(0,stream.beg);

    std::vector<char> bin(size);
    stream.read(bin.data(), size);

    return bin;
}

inline void drop_uncompleted_task(xclDeviceHandle handle, task_info &cmd) {
    if (cmd.desc != NULL)
        munmap(cmd.desc, 4096);

    if (cmd.ecmd != NULL)
        munmap(cmd.ecmd, 4096);

    if (cmd.buf_boh != NULLBO)
        xclFreeBO(handle, cmd.buf_boh);

    if (cmd.desc_bo != NULLBO)
        xclFreeBO(handle, cmd.desc_bo);

    if (cmd.exec_bo != NULLBO)
        xclFreeBO(handle, cmd.exec_bo);
}

inline void start_fa_kernel(xclDeviceHandle handle, int cu_idx, uint64_t desc_addr)
{
    static uint32_t msb = 0;

    if (msb != desc_addr >> 32) {
        //std::cout << "Desc base addr 0x" << std::hex << desc_addr << std::dec << std::endl;
        //std::cout << "Writing 0x" << std::hex << (desc_addr >> 32) << std::dec << " to 0x00 register" << std::endl;
        /* 0x00 nextDescriptorAddr_MSW register
         * This register doesn't need to change in each kick off
         */
        xclRegWrite(handle, cu_idx, 0x00, desc_addr >> 32);
        msb = desc_addr >> 32;
    }

    //std::cout << "Writing 0x" << std::hex << static_cast<int>(desc_addr) << std::dec << " to 0x04 register" << std::endl;
    /* **  Write to the LSW register will trigger the exectuion ** */
    /* 0x04 nextDescriptorAddr_LSW register */
    xclRegWrite(handle, cu_idx, 0x04, desc_addr);
}

double runTest(xclDeviceHandle handle, std::vector<std::shared_ptr<task_info>>& cmds,
               unsigned int total)
{
    /* TODO: The IP name would change. It determines by final xclbin */
    auto cu_idx = xclIPName2Index(handle, "FA_Increment:FA_Increment_1");
    int cmd_idx = 0;
    uint32_t submitted = 0;
    uint32_t completed = 0;
    auto start = std::chrono::high_resolution_clock::now();

    while (submitted < DESC_FIFO_DEPTH) {
        start_fa_kernel(handle, cu_idx, cmds[submitted]->desc_paddr);
        submitted++;
        if (submitted == total || submitted == cmds.size())
            break;
    }

    while (completed < total) {
        if (cmds[cmd_idx]->desc->status == fa::COMPLETED) {

            /* Process completed command here ... */

            completed++;
            cmds[cmd_idx]->desc->status = fa::ISSUED;
            if (++cmd_idx == cmds.size())
                cmd_idx = 0;

            /* If we still have unsubmitted commands, it is because FIFO was full.
             * Now, FIFO must be not full. It is time to send one more comands to the hardware.
             */
            if (submitted < total) {
                start_fa_kernel(handle, cu_idx, cmds[submitted % cmds.size()]->desc_paddr);
                //std::cout << "boh_addr 0x" << std::hex << boh_addr << std::dec << std::endl;
                submitted++;
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    return (std::chrono::duration_cast<std::chrono::microseconds>(end - start)).count();
}

int run_test(xclDeviceHandle handle, xuid_t uuid, int bank)
{
    std::vector<std::shared_ptr<task_info>> cmds;
    //std::vector<unsigned int> cmds_per_run = { 4, 8, 16, 32 };
    std::vector<unsigned int> cmds_per_run = { 1000, 10000, 50000, 100000 };
    int expected_cmds = 100;
    int size;

    /* descriptor size is kernel specific
     * Since descEntry and descriptor are variable size, needs pre calculate the size.
     */
    size = get_desc_size();

    std::cout << "descriptor size " << size << std::endl;

    auto cu_idx = xclIPName2Index(handle, "FA_Increment:FA_Increment_1");
    if (xclOpenContext(handle, uuid, cu_idx, false))
        throw std::runtime_error("Cound not open context");

    for (int i = 0; i < expected_cmds; i++) {
        task_info cmd = {NULLBO, NULLBO, NULLBO, 0, NULL, NULL};
        xclBOProperties prop;
        fa::descEntry_t *entry;
        uint64_t boh_addr;
        uint32_t len;
        int off;

        cmd.buf_boh = xclAllocBO(handle, 4096, 0, bank);
        if (cmd.buf_boh == NULLBO) {
            std::cout << "xclAllocBO failed" << std::endl;
            break;
        }
        uint32_t *input = reinterpret_cast<uint32_t *>(xclMapBO(handle, cmd.buf_boh, true));
        for (int j = 0; j < 1024; j++) {
            input[j] = j;
        }
        xclSyncBO(handle, cmd.buf_boh, XCL_BO_SYNC_BO_TO_DEVICE, 4096, 0);

        /* PLRAM is bank 1 */
        cmd.desc_bo = xclAllocBO(handle, size, 0, XCL_BO_FLAGS_P2P | 0x0);
        if (cmd.desc_bo == NULLBO) {
            std::cout << "xclAllocBO failed" << std::endl;
            drop_uncompleted_task(handle, cmd);
            break;
        }
        cmd.desc = reinterpret_cast<fa::descriptor_t *>(xclMapBO(handle, cmd.desc_bo, true));
        if (cmd.desc == MAP_FAILED) {
            drop_uncompleted_task(handle, cmd);
            break;
        }
        xclGetBOProperties(handle, cmd.desc_bo, &prop);
        cmd.desc_paddr = prop.paddr;

        /* --- Construct descriptor --- */
        cmd.desc->status = fa::ISSUED;
        cmd.desc->numInputEntries = 1;
        //cmd.desc->inputEntryBytes = get_input_entries_size();
        cmd.desc->inputEntryBytes = 8;
        cmd.desc->numOutputEntries = 0;
        cmd.desc->outputEntryBytes = get_output_entries_size();

        off = 0;
        /* Entry for mem_OFFSET */
        xclGetBOProperties(handle, cmd.buf_boh, &prop);
        boh_addr = prop.paddr;
        entry = reinterpret_cast<fa::descEntry_t *>(cmd.desc->data + off);
        entry->argOffset = 0x10;
        entry->argSize = sizeof(boh_addr);
        /* arrValue[] is aligned by bytes, use memcpy() */
        memcpy(entry->argValue, &boh_addr, entry->argSize);
        off += (sizeof(fa::descEntry_t) + entry->argSize)/4;
        /* --- End Construct descriptor --- */

        //print_descriptor(cmd.desc);

        cmds.push_back(std::make_shared<task_info>(cmd));
    }
    /* Maybe the manchine is not able to allocate BO for all commands.
     * In this case, the cmds.size() is less than expected_cmds
     * After a command finished, re-send the command.
     */
    std::cout << "Allocated commands, expect " << expected_cmds << ", created " << cmds.size() << std::endl;

    for (auto &num_cmds : cmds_per_run) {
        auto duration = runTest(handle, cmds, num_cmds);
        std::cout << "Commands: " << std::setw(7) << num_cmds
            << " iops: " << (num_cmds * 1000.0 * 1000.0 / duration)
            << std::endl;
    }

    for (auto &cmd : cmds) {
        munmap(cmd->desc, 4096);
        munmap(cmd->ecmd, 4096);
        xclFreeBO(handle, cmd->buf_boh);
        xclFreeBO(handle, cmd->desc_bo);
        xclFreeBO(handle, cmd->exec_bo);
    }

    xclCloseContext(handle, uuid, cu_idx);
    return 0;
}

int _main(int argc, char* argv[])
{
    xclDeviceHandle handle;
    std::string xclbin_fn;
    int device_id = 0;
    xuid_t uuid;
    int first_mem = 0;
    char c;

    while ((c = getopt(argc, argv, "k:d:h")) != -1) {
        switch (c) {
            case 'k':
               xclbin_fn = optarg;
               break;
            case 'd':
               device_id = std::atoi(optarg);
               break;
            case 'h':
               usage();
        }
    }

    if (xclbin_fn.empty())
        throw std::runtime_error("No xclbin");

    printf("The system has %d device(s)\n", xclProbe());

    handle = xclOpen(device_id, "", XCL_QUIET);
    if (!handle) {
        printf("Could not open device\n");
        return 1;
    }

    auto xclbin = load_file_to_memory(xclbin_fn);
    auto top = reinterpret_cast<const axlf*>(xclbin.data());
    auto topo = xclbin::get_axlf_section(top, MEM_TOPOLOGY);
    auto topology = reinterpret_cast<mem_topology*>(xclbin.data() + topo->m_sectionOffset);
    if (xclLoadXclBin(handle, top))
        throw std::runtime_error("Bitstream download failed");

    uuid_copy(uuid, top->m_header.uuid);

    for (int i = 0; i < topology->m_count; ++i) {
        if (topology->m_mem_data[i].m_used) {
            first_mem = i;
            break;
        }
    }

    printf("Download bitstream done\n");
    run_test(handle, uuid, first_mem);

    xclClose(handle);
    return 0;
}

int main(int argc, char *argv[])
{
    try {
        _main(argc, argv);
        return 0;
    }
    catch (const std::exception& ex) {
        std::cout << "TEST FAILED: " << ex.what() << std::endl;
    }
    catch (...) {
        std::cout << "TEST FAILED" << std::endl;
    }

    return 1;
};
