/**
 * @file remote_cpu_info.c
 *
 * Popcorn Linux remote cpuinfo implementation
 * This work is a rework of Akshay Giridhar and Sharath Kumar Bhat's
 * implementation to provide the proc/cpuinfo for remote cores.
 *
 * @author Jingoo Han, SSRG Virginia Tech 2017
 */

#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include <popcorn/bundle.h>
#include <popcorn/cpuinfo.h>
#include <popcorn/pcn_kmsg.h>

#include "wait_station.h"

#define REMOTE_CPUINFO_VERBOSE 0
#if REMOTE_CPUINFO_VERBOSE
#define CPUPRINTK(...) printk(__VA_ARGS__)
#else
#define CPUPRINTK(...)
#endif

#define REMOTE_CPUINFO_MESSAGE_FIELDS \
	struct _remote_cpu_info_data cpu_info_data; \
	int nid; \
	int origin_ws;
DEFINE_PCN_KMSG(remote_cpu_info_data_t, REMOTE_CPUINFO_MESSAGE_FIELDS);

static struct _remote_cpu_info_data *saved_cpu_info[MAX_POPCORN_NODES];

void send_remote_cpu_info_request(unsigned int nid)
{
	remote_cpu_info_data_t *request;
	remote_cpu_info_data_t *response;
	struct wait_station *ws = get_wait_station(current);

	CPUPRINTK("%s: Entered, nid: %d\n", __func__, nid);

	request = kzalloc(sizeof(*request), GFP_KERNEL);

	/* 1. Construct request data to send it into remote node */

	/* 1-1. Fill the header information */
	request->header.type = PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_REQUEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;
	request->nid = my_nid;
	request->origin_ws = ws->id;

	/* 1-2. Fill the machine-dependent CPU infomation */
	fill_cpu_info(&request->cpu_info_data);

	/* 1-3. Send request into remote node */
	pcn_kmsg_send(nid, request, sizeof(*request));

	/* 2. Request message should wait until response message is done. */
	response = wait_at_station(ws);
	put_wait_station(ws);

	memcpy(saved_cpu_info[nid], &response->cpu_info_data,
	       sizeof(response->cpu_info_data));

	kfree(request);
	pcn_kmsg_free_msg(response);

	CPUPRINTK("%s: done\n", __func__);
}

unsigned int get_number_cpus_from_remote_node(unsigned int nid)
{
	unsigned int num_cpus = 0;

	switch (saved_cpu_info[nid]->arch_type) {
	case arch_x86:
		num_cpus = saved_cpu_info[nid]->arch.x86.num_cpus;
		break;
	case arch_arm:
		num_cpus = saved_cpu_info[nid]->arch.arm64.num_cpus;
		break;
	default:
		CPUPRINTK("%s: Unknown CPU\n", __func__);
		num_cpus = 0;
		break;
	}

	return num_cpus;
}

static int handle_remote_cpu_info_request(struct pcn_kmsg_message *inc_msg)
{
	remote_cpu_info_data_t *request;
	remote_cpu_info_data_t *response;
	int ret;

	CPUPRINTK("%s: Entered\n", __func__);

	request = (remote_cpu_info_data_t *)inc_msg;

	response = kzalloc(sizeof(*response), GFP_KERNEL);
	if (!response) return -ENOMEM;

	/* 1. Save remote cpu info from remote node */
	memcpy(saved_cpu_info[request->nid],
	       &request->cpu_info_data, sizeof(request->cpu_info_data));

	/* 2. Construct response data to send it into remote node */

	/* 2-1. Fill the header information */
	response->header.type = PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_RESPONSE;
	response->header.prio = PCN_KMSG_PRIO_NORMAL;
	response->nid = my_nid;
	response->origin_ws = request->origin_ws;

	/* 2-2. Fill the machine-dependent CPU infomation */
	ret = fill_cpu_info(&response->cpu_info_data);
	if (ret < 0) {
		CPUPRINTK("%s: failed to fill cpu info\n", __func__);
		goto out;
	}

	/* 2-3. Send response into remote node */
	ret = pcn_kmsg_send(request->nid, response, sizeof(*response));
	if (ret < 0) {
		CPUPRINTK("%s: failed to send response message\n", __func__);
		goto out;
	}

	/* 3. Remove request message received from remote node */
	pcn_kmsg_free_msg(request);

	CPUPRINTK("%s: done\n", __func__);
out:
	kfree(response);
	return 0;
}

static int handle_remote_cpu_info_response(struct pcn_kmsg_message *inc_msg)
{
	remote_cpu_info_data_t *response;
	struct wait_station *ws;

	CPUPRINTK("%s: Entered\n", __func__);

	response = (remote_cpu_info_data_t *)inc_msg;

	ws = wait_station(response->origin_ws);
	ws->private = response;

	smp_mb();

	if (atomic_dec_and_test(&ws->pendings_count))
		complete(&ws->pendings);

	CPUPRINTK("%s: done\n", __func__);
	return 0;
}

int remote_cpu_info_init(void)
{
	int i = 0;

	/* Allocate the buffer for saving remote CPU info */
	for (i = 0; i < MAX_POPCORN_NODES; i++)
		saved_cpu_info[i] = kzalloc(sizeof(struct _remote_cpu_info_data),
					    GFP_KERNEL);

	/* Register callbacks for both request and response */
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_REQUEST,
				   handle_remote_cpu_info_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_RESPONSE,
				   handle_remote_cpu_info_response);

	CPUPRINTK("%s: done\n", __func__);
	return 0;
}

static void print_x86_cpuinfo(struct seq_file *m,
		       struct _remote_cpu_info_data *data,
		       int count)
{
	seq_printf(m, "processor\t: %u\n", data->arch.x86.cpu[count]._processor);
	seq_printf(m, "vendor_id\t: %s\n", data->arch.x86.cpu[count]._vendor_id);
	seq_printf(m, "cpu_family\t: %d\n", data->arch.x86.cpu[count]._cpu_family);
	seq_printf(m, "model\t\t: %u\n", data->arch.x86.cpu[count]._model);
	seq_printf(m, "model name\t: %s\n", data->arch.x86.cpu[count]._model_name);

	if (data->arch.x86.cpu[count]._stepping != -1)
		seq_printf(m, "stepping\t: %d\n", data->arch.x86.cpu[count]._stepping);
	else
		seq_puts(m, "stepping\t: unknown\n");

	seq_printf(m, "microcode\t: 0x%lx\n", data->arch.x86.cpu[count]._microcode);
	seq_printf(m, "cpu MHz\t\t: %u\n", data->arch.x86.cpu[count]._cpu_freq);
	seq_printf(m, "cache size\t: %d kB\n", data->arch.x86.cpu[count]._cache_size);
	seq_puts(m, "flags\t\t:");
	seq_printf(m, " %s", data->arch.x86.cpu[count]._flags);
	seq_printf(m, "\nbogomips\t: %lu\n", data->arch.x86.cpu[count]._nbogomips);
	seq_printf(m, "TLB size\t: %d 4K pages\n", data->arch.x86.cpu[count]._TLB_size);
	seq_printf(m, "clflush size\t: %u\n", data->arch.x86.cpu[count]._clflush_size);
	seq_printf(m, "cache_alignment\t: %d\n", data->arch.x86.cpu[count]._cache_alignment);
	seq_printf(m, "address sizes\t: %u bits physical, %u bits virtual\n",
		   data->arch.x86.cpu[count]._bits_physical,
		   data->arch.x86.cpu[count]._bits_virtual);
}

static void print_arm_cpuinfo(struct seq_file *m,
		       struct _remote_cpu_info_data *data,
		       int count)
{
	seq_printf(m, "processor\t: %u\n", data->arch.arm64.percore[count].processor_id);

	if (data->arch.arm64.percore[count].compat)
		 seq_printf(m, "model name\t: %s %d (%s)\n",
			    data->arch.arm64.percore[count].model_name,
			    data->arch.arm64.percore[count].model_rev,
			    data->arch.arm64.percore[count].model_elf);
	else
		 seq_printf(m, "model name\t: %s\n",
			    data->arch.arm64.percore[count].model_name);

	seq_printf(m, "BogoMIPS\t: %lu.%02lu\n",
		   data->arch.arm64.percore[count].bogo_mips,
		   data->arch.arm64.percore[count].bogo_mips_fraction);
	seq_puts(m, "Features\t:");
	seq_printf(m, " %s", data->arch.arm64.percore[count].flags);
	seq_puts(m, "\n");

	seq_printf(m, "CPU implementer\t: 0x%02x\n", data->arch.arm64.percore[count].cpu_implementer);
	seq_printf(m, "CPU architecture: %d\n", data->arch.arm64.percore[count].cpu_archtecture);
	seq_printf(m, "CPU variant\t: 0x%x\n", data->arch.arm64.percore[count].cpu_variant);
	seq_printf(m, "CPU part\t: 0x%03x\n", data->arch.arm64.percore[count].cpu_part);
	seq_printf(m, "CPU revision\t: %d\n", data->arch.arm64.percore[count].cpu_revision);

	return;
}

static void print_unknown_cpuinfo(struct seq_file *m)
{
	seq_puts(m, "processor\t: Unknown\n");
	seq_puts(m, "vendor_id\t: Unknown\n");
	seq_puts(m, "cpu_family\t: Unknown\n");
	seq_puts(m, "model\t\t: Unknown\n");
	seq_puts(m, "model name\t: Unknown\n");
}

int remote_proc_cpu_info(struct seq_file *m, unsigned int nid, unsigned int vpos)
{
	seq_printf(m, "****    Remote CPU at %d   ****\n", nid);

	switch (saved_cpu_info[nid]->arch_type) {
	case arch_x86:
		print_x86_cpuinfo(m, saved_cpu_info[nid], vpos);
		break;
	case arch_arm:
		print_arm_cpuinfo(m, saved_cpu_info[nid], vpos);
		break;
	default:
		print_unknown_cpuinfo(m);
		break;
	}

	seq_puts(m, "\n");
	return 0;
}
