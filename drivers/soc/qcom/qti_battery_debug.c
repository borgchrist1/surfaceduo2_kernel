// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"BATTERY_DBG: %s: " fmt, __func__

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/soc/qcom/pmic_glink.h>

/* owner/type/opcodes for battery debug */
#define MSG_OWNER_BD			32781
#define MSG_OWNER_BC			32778  //MSCHANGE
#define BATTMAN_LOG_LEVEL_DEBUG  4 //MSCHANGE
#define ChargerPDLoggingCategories 8405537 //MSCHANGE
#define MS_SET_LOGGING_PROP_REQ  0x19  //MSCHANGE
#define MSG_TYPE_REQ_RESP		1
#define BD_GET_AGGREGATOR_INFO_REQ	0x15
#define BD_OVERWRITE_VOTABLE_REQ	0x16
#define BD_GET_VOTABLE_REQ		0x17
#define BD_QBG_DUMP_REQ			0x36

/* Generic definitions */
#define MAX_BUF_LEN		SZ_4K
#define BD_WAIT_TIME_MS		1000

#define MAX_NUM_VOTABLES	12
#define MAX_NUM_VOTERS		32
#define MAX_NAME_LEN		12

// MSCHANGE Start
#define MS_DEFAULT_ADSP_LOG_FETCH_TIME_INTERVAL  60 // 1 minute
#define BATT_MNGR_GET_ULOG_REQ		0x0018
#define MAX_ULOG_READ_BUFFER_SIZE	8192			// 8MB
#define MAX_MSG_LENGTH				251

/** request Message; to get ulogs from chargerPD */
struct battman_get_log_req_msg{
  struct pmic_glink_hdr hdr;
  u32 max_logsize;
};

/** Response Message; to get ulogs from chargerPD */
struct battman_get_log_resp_msg{
  struct pmic_glink_hdr hdr;
  char read_buffer[MAX_ULOG_READ_BUFFER_SIZE];
};

struct set_log_properties_req_msg{
  struct pmic_glink_hdr hdr;
  u64 	categories;
  u32 	level;
};
// MSCHANGE End

struct all_votables_data {
	u32			num_votables;
	u32			num_voters;
	char			votables[MAX_NUM_VOTABLES][MAX_NAME_LEN];
	char			voters[MAX_NUM_VOTERS][MAX_NAME_LEN];
};

struct votable_data {
	u32				votable_id;
	u32				eff_val;
	u8				voter_ids[MAX_NUM_VOTERS]; /* unused */
	u32				votes[MAX_NUM_VOTERS];
	u32				active_voter_mask;
	u32				eff_voter;
	u32				override_voter;
};

struct votable {
	char				*name;
	u32				id;
	struct battery_dbg_dev		*bd;
	struct votable_data		data;
	u32				override_val;
};

struct qbg_context_req_msg {
	struct pmic_glink_hdr hdr;
};

struct qbg_context_resp_msg {
	struct pmic_glink_hdr		hdr;
	u32				length;
	u8				buf[MAX_BUF_LEN];
};

struct votables_list_req_msg {
	struct pmic_glink_hdr hdr;
};

struct votables_list_resp_msg {
	struct pmic_glink_hdr		hdr;
	struct all_votables_data	all_data;
};

struct votable_req_msg {
	struct pmic_glink_hdr		hdr;
	u32				votable_id;
};

struct votable_resp_msg {
	struct pmic_glink_hdr		hdr;
	struct votable_data		v_data;
};

struct override_req_msg {
	struct pmic_glink_hdr		hdr;
	u32				votable_id;
	u32				override_val;
};

struct battery_dbg_dev {
	struct device			*dev;
	struct pmic_glink_client	*client;
	struct mutex			lock;
	struct completion		ack;
	struct qbg_context_resp_msg	qbg_dump;
	struct dentry			*debugfs_dir;
	struct debugfs_blob_wrapper	qbg_blob;
	struct all_votables_data	all_data;
	struct votable			*votable;
	u8				override_voter_id;
	struct workqueue_struct     *ms_adsp_log_fetch_wq; //MSCHANGE start
	struct delayed_work			ms_adsp_log_fetch_work;
	u16 						ms_adsp_log_fetch_time_interval; //MSCHANGE end
};

static int battery_dbg_write(struct battery_dbg_dev *bd, void *data, size_t len)
{
	int rc;

	mutex_lock(&bd->lock);
	reinit_completion(&bd->ack);
	rc = pmic_glink_write(bd->client, data, len);
	if (!rc) {
		rc = wait_for_completion_timeout(&bd->ack,
					msecs_to_jiffies(BD_WAIT_TIME_MS));
		if (!rc) {
			pr_err("Error, timed out sending message\n");
			mutex_unlock(&bd->lock);
			return -ETIMEDOUT;
		}

		rc = 0;
	}
	mutex_unlock(&bd->lock);

	return rc;
}

// MSCHANGE Start
static void parse_chargerPD_logs(char * received_buff)
{
	size_t received_buff_size = 0;
	char received_buff_modified[256];
	u32 buff_index = 0, counter = 0, copy_size = 0;

	received_buff_size= strlen(received_buff);

	if(received_buff_size == 0){
	  pr_debug("ChargerPD Ulog is Empty");
	  return;
    }

    pr_debug("****Received ChargerPD Logs of size %d****", received_buff_size);

    while(counter < received_buff_size){
      if(received_buff[counter] == '\n' || received_buff[counter] == '\0'){
        copy_size = (counter - buff_index) + 1;
        if(copy_size<MAX_MSG_LENGTH){
          memset(received_buff_modified, '\0', 256);
          strlcpy(received_buff_modified, &received_buff[buff_index], copy_size); //Strlcpy copies 1 byte less than the #of bytes specified
          pr_info("%s", received_buff_modified);  //(copy_size+1) will copy '\n' from the ulog. So no need for including '\n' while printing
		  buff_index += copy_size;
        }
        else{
          while(buff_index < counter){
            copy_size = (copy_size > MAX_MSG_LENGTH) ? MAX_MSG_LENGTH : (counter - buff_index)+1;
            memset(received_buff_modified, '\0', 256);
            if(copy_size<256)
            {
              strlcpy(received_buff_modified, &received_buff[buff_index], copy_size); //Strlcpy copies 1 byte less than the #of bytes specified
              pr_info("%s", received_buff_modified);
              buff_index += (copy_size-1);
            }
            copy_size = (counter - buff_index)+1;
          }
        }
      }
      counter++;
    }
}

static void handle_get_logs_message(struct battery_dbg_dev *bd,
						struct battman_get_log_resp_msg *resp_msg,
						size_t len)
{
	if (len != sizeof(*resp_msg)) {
		pr_err("Expected data length: %zu, received: %zu\n",
				sizeof(*resp_msg), len);
		return;
	}
	parse_chargerPD_logs(resp_msg->read_buffer);
	complete(&bd->ack);
}

static int battery_dbg_get_logs(struct battery_dbg_dev *bd)
{
	struct battman_get_log_req_msg req_msg = { {0} };
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BATT_MNGR_GET_ULOG_REQ;
	req_msg.max_logsize = MAX_ULOG_READ_BUFFER_SIZE;

	return battery_dbg_write(bd, &req_msg, sizeof(req_msg));
}

static int write_log_categories(void *data , u64 val){
	struct battery_dbg_dev *bd = data;
	struct set_log_properties_req_msg req_msg = { { 0 } };

	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = MS_SET_LOGGING_PROP_REQ;
	req_msg.categories = val;
	req_msg.level = BATTMAN_LOG_LEVEL_DEBUG;

	return battery_dbg_write(bd, &req_msg, sizeof(req_msg));
}

static int log_categories_write(void *data, u64 val)
{
	return write_log_categories(data,val);
}

DEFINE_SIMPLE_ATTRIBUTE(log_categories_ops, NULL,
			log_categories_write, "%d\n");

static void ms_adsp_log_fetch_work_function(struct work_struct *work) {
	struct battery_dbg_dev *bd = container_of(work, struct battery_dbg_dev, ms_adsp_log_fetch_work.work);
	battery_dbg_get_logs(bd);
	queue_delayed_work(bd->ms_adsp_log_fetch_wq, &bd->ms_adsp_log_fetch_work, bd->ms_adsp_log_fetch_time_interval * HZ);
}
// MSCHANGE End

static void handle_qbg_dump_message(struct battery_dbg_dev *bd,
				    struct qbg_context_resp_msg *resp_msg,
				    size_t len)
{
	u32 buf_len;

	if (len > sizeof(bd->qbg_dump)) {
		pr_err("Incorrect length received: %zu expected: %u\n", len,
			sizeof(bd->qbg_dump));
		return;
	}

	buf_len = resp_msg->length;
	if (buf_len > sizeof(bd->qbg_dump.buf)) {
		pr_err("Incorrect buffer length: %u\n", buf_len);
		return;
	}

	pr_debug("buf length: %u\n", buf_len);
	memcpy(bd->qbg_dump.buf, resp_msg->buf, buf_len);
	bd->qbg_blob.size = buf_len;

	complete(&bd->ack);
}

static void handle_override_message(struct battery_dbg_dev *bd, void *unused,
				    size_t len)
{
	pr_debug("override succeeded\n");
	complete(&bd->ack);
}

static void handle_get_votable_message(struct battery_dbg_dev *bd,
				       struct votable_resp_msg *resp_msg,
				       size_t len)
{
	u32 id = resp_msg->v_data.votable_id;

	if (len != sizeof(*resp_msg)) {
		pr_err("Expected data length: %zu, received: %zu\n",
				sizeof(*resp_msg), len);
		return;
	}

	if (id >= MAX_NUM_VOTABLES) {
		pr_err("Votable id %u exceeds max %d\n", id, MAX_NUM_VOTABLES);
		return;
	}

	if (resp_msg->v_data.active_voter_mask &&
			resp_msg->v_data.eff_voter >= MAX_NUM_VOTERS) {
		pr_err("Effective voter id %u exceeds max %d\n",
				resp_msg->v_data.eff_voter, MAX_NUM_VOTERS);
		return;
	}

	memcpy(&bd->votable[id].data, &resp_msg->v_data,
			sizeof(resp_msg->v_data));

	complete(&bd->ack);
}

#define OVERRIDE_VOTER_NAME	"glink"
static void handle_get_votables_list_message(struct battery_dbg_dev *bd,
				       struct votables_list_resp_msg *resp_msg,
				       size_t len)
{
	u8 i;

	if (len != sizeof(*resp_msg)) {
		pr_err("Expected data length: %zu, received: %zu\n",
				sizeof(*resp_msg), len);
		return;
	}

	if (resp_msg->all_data.num_votables >= MAX_NUM_VOTABLES) {
		pr_err("Num votables %u exceeds max %d\n",
		       resp_msg->all_data.num_votables, MAX_NUM_VOTABLES);
		return;
	}

	if (resp_msg->all_data.num_voters >= MAX_NUM_VOTERS) {
		pr_err("Num voters %u exceeds max %d\n",
		       resp_msg->all_data.num_voters, MAX_NUM_VOTERS);
		return;
	}

	memcpy(&bd->all_data, &resp_msg->all_data, sizeof(resp_msg->all_data));

	for (i = 0; i < MAX_NUM_VOTABLES; i++)
		bd->all_data.votables[i][MAX_NAME_LEN - 1] = '\0';

	for (i = 0; i < MAX_NUM_VOTERS; i++)
		bd->all_data.voters[i][MAX_NAME_LEN - 1] = '\0';

	if (!bd->override_voter_id) {
		for (i = 0; i < MAX_NUM_VOTERS; i++) {
			if (!strcmp(bd->all_data.voters[i],
						OVERRIDE_VOTER_NAME)) {
				bd->override_voter_id = i;
				break;
			}
		}
	}

	complete(&bd->ack);
}

static int battery_dbg_callback(void *priv, void *data, size_t len)
{
	struct pmic_glink_hdr *hdr = data;
	struct battery_dbg_dev *bd = priv;

	pr_debug("owner: %u type: %u opcode: %#x len: %zu\n", hdr->owner,
		hdr->type, hdr->opcode, len);

	switch (hdr->opcode) {
	case BD_QBG_DUMP_REQ:
		handle_qbg_dump_message(bd, data, len);
		break;
	case BD_GET_AGGREGATOR_INFO_REQ:
		handle_get_votables_list_message(bd, data, len);
		break;
	case BD_GET_VOTABLE_REQ:
		handle_get_votable_message(bd, data, len);
		break;
	case BD_OVERWRITE_VOTABLE_REQ:
		handle_override_message(bd, data, len);
		break;
	// MSCHANGE Start
	case BATT_MNGR_GET_ULOG_REQ:
		// Handle the logs
		handle_get_logs_message(bd, data, len);
		break;
	case MS_SET_LOGGING_PROP_REQ:
		complete(&bd->ack);
	break;
	// MSCHANGE End
	default:
		pr_err("Unknown opcode %u\n", hdr->opcode);
		break;
	}

	return 0;
}

static int get_qbg_context_write(void *data, u64 val)
{
	struct battery_dbg_dev *bd = data;
	struct qbg_context_req_msg req_msg = { { 0 } };

	req_msg.hdr.owner = MSG_OWNER_BD;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BD_QBG_DUMP_REQ;

	return battery_dbg_write(bd, &req_msg, sizeof(req_msg));
}

#ifdef CONFIG_DEBUG_FS
static int battery_dbg_request_read_votable(struct battery_dbg_dev *bd,
					    u32 id)
{
	struct votable_req_msg req_msg = { { 0 } };

	req_msg.hdr.owner = MSG_OWNER_BD;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BD_GET_VOTABLE_REQ;
	req_msg.votable_id = id;

	return battery_dbg_write(bd, &req_msg, sizeof(req_msg));
}

#ifdef CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG
static int battery_dbg_request_override(struct battery_dbg_dev *bd, u32 id,
					u32 val)
{
	struct override_req_msg req_msg = { { 0 } };

	req_msg.hdr.owner = MSG_OWNER_BD;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BD_OVERWRITE_VOTABLE_REQ;
	req_msg.votable_id = id;
	req_msg.override_val = val;

	pr_debug("requesting override of %s with value %u\n",
			bd->votable[id].name, val);

	return battery_dbg_write(bd, &req_msg, sizeof(req_msg));
}
#endif

static int active_show(struct seq_file *s, void *unused)
{
	int rc;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	seq_printf(s, "%#x\n", voter_mask);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(active);

static int winvote_show(struct seq_file *s, void *unused)
{
	int rc, winvote;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;
	winvote = voter_mask ? v->data.eff_val : -EINVAL;

	seq_printf(s, "%d\n", winvote);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(winvote);

static int winner_show(struct seq_file *s, void *unused)
{
	int rc;
	char *winner;
	u32 eff_voter;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	if (voter_mask) {
		eff_voter = v->data.eff_voter;
		winner = bd->all_data.voters[eff_voter];
	} else {
		winner = "";
	}

	seq_printf(s, "%s\n", winner);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(winner);

static int voters_show(struct seq_file *s, void *unused)
{
	int rc, i;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	for_each_set_bit(i, &voter_mask, MAX_NUM_VOTERS)
		seq_printf(s, "%s ", bd->all_data.voters[i]);
	seq_puts(s, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(voters);

static int votes_show(struct seq_file *s, void *unused)
{
	int rc, i;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	for_each_set_bit(i, &voter_mask, MAX_NUM_VOTERS)
		seq_printf(s, "%d ", v->data.votes[i]);
	seq_puts(s, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(votes);

static int status_show(struct seq_file *s, void *unused)
{
	int rc, i, winvote;
	char *winner;
	u32 eff_voter;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	if (!voter_mask) {
		seq_puts(s, "\n");
		return 0;
	}

	winvote = v->data.eff_val;
	eff_voter = v->data.eff_voter;
	winner = bd->all_data.voters[eff_voter];

	for_each_set_bit(i, &voter_mask, MAX_NUM_VOTERS)
		seq_printf(s, "           %-*s: %-*s: %d\n", MAX_NAME_LEN,
				v->name, MAX_NAME_LEN, bd->all_data.voters[i],
				v->data.votes[i]);
	seq_printf(s, "EFFECTIVE: %-*s: %-*s: %d\n", MAX_NAME_LEN, v->name,
			MAX_NAME_LEN, winner, winvote);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

#ifdef CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG
static int override_get(void *data, u64 *val)
{
	int rc;
	struct votable *v = data;
	struct battery_dbg_dev *bd = v->bd;
	unsigned long voter_mask;

	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	/* Show override voter's vote only if override voter is active */
	if (test_bit(bd->override_voter_id, &voter_mask))
		*val = v->data.votes[bd->override_voter_id];
	else
		*val = 0;

	return 0;
}

static int override_set(void *data, u64 val)
{
	int rc;
	struct votable *v = data;
	struct battery_dbg_dev *bd = v->bd;
	u32 set = val;

	rc = battery_dbg_request_override(bd, v->id, set);
	if (rc) {
		pr_err("%s override request failed: %d\n", v->name, rc);
		return rc;
	}

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(override_fops, override_get, override_set, "%llu\n");

static int battery_dbg_create_override_file(struct votable *v,
					    struct dentry *votable_dir)
{
	return PTR_ERR_OR_ZERO(debugfs_create_file_unsafe("override", 0600,
				votable_dir, v, &override_fops));

}
#else
static int battery_dbg_create_override_file(struct votable *v,
					    struct dentry *votable_dir)
{
	return 0;
}
#endif

static int battery_dbg_create_votable(struct battery_dbg_dev *bd,
				      struct dentry *votables_root_dir,
				      u32 id)
{
	int rc;
	char *v_name = bd->all_data.votables[id];
	struct dentry *votable_dir;

	votable_dir = debugfs_create_dir(v_name, votables_root_dir);
	if (IS_ERR(votable_dir)) {
		pr_err("Failed to create %s debugfs directory\n", v_name);
		return PTR_ERR(votable_dir);
	}

	bd->votable[id].name = v_name;

	rc = PTR_ERR_OR_ZERO(debugfs_create_file("active", 0400, votable_dir,
			&bd->votable[id], &active_fops));
	if (rc)
		goto error;

	rc = PTR_ERR_OR_ZERO(debugfs_create_file("winvote", 0400, votable_dir,
			&bd->votable[id], &winvote_fops));
	if (rc)
		goto error;

	rc = PTR_ERR_OR_ZERO(debugfs_create_file("winner", 0400, votable_dir,
			&bd->votable[id], &winner_fops));
	if (rc)
		goto error;

	rc = PTR_ERR_OR_ZERO(debugfs_create_file("voters", 0400, votable_dir,
			&bd->votable[id], &voters_fops));
	if (rc)
		goto error;

	rc = PTR_ERR_OR_ZERO(debugfs_create_file("votes", 0400, votable_dir,
			&bd->votable[id], &votes_fops));
	if (rc)
		goto error;

	rc = PTR_ERR_OR_ZERO(debugfs_create_file("status", 0400, votable_dir,
			&bd->votable[id], &status_fops));
	if (rc)
		goto error;

	rc = battery_dbg_create_override_file(&bd->votable[id], votable_dir);
	if (rc)
		goto error;

	return 0;

error:
	pr_err("Failed to create debugfs file: %d\n", rc);
	return rc;
}

static int battery_dbg_get_votables_list(struct battery_dbg_dev *bd)
{
	struct votable_req_msg req_msg = { { 0 } };

	req_msg.hdr.owner = MSG_OWNER_BD;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BD_GET_AGGREGATOR_INFO_REQ;

	return battery_dbg_write(bd, &req_msg, sizeof(req_msg));
}

static int battery_dbg_create_votables(struct battery_dbg_dev *bd,
				       struct dentry *bd_root_dir)
{
	int rc, id;
	u32 num_votables;
	struct dentry *votables_root_dir;

	votables_root_dir = debugfs_create_dir("votables", bd_root_dir);
	if (IS_ERR(votables_root_dir)) {
		pr_err("Failed to create votables root directory\n");
		return PTR_ERR(votables_root_dir);
	}

	rc = battery_dbg_get_votables_list(bd);
	if (rc) {
		pr_err("Failed to get votables list: %d\n", rc);
		return rc;
	}

	num_votables = bd->all_data.num_votables;

	bd->votable = devm_kcalloc(bd->dev, num_votables,
				   sizeof(struct votable), GFP_KERNEL);
	if (!bd->votable)
		return -ENOMEM;

	for (id = 0; id < num_votables; id++) {
		bd->votable[id].bd = bd;
		bd->votable[id].id = id;
		rc = battery_dbg_create_votable(bd, votables_root_dir, id);
		if (rc)
			return rc;
	}

	return 0;
}

// MSCHANGE start
static int ms_adsp_log_fetch_time_interval_read(void *data , u64 *val) {
	struct battery_dbg_dev *bd = data;
	*val = bd->ms_adsp_log_fetch_time_interval;
	return 0;
}

static int ms_adsp_log_fetch_time_interval_write(void *data, u64 val) {
	struct battery_dbg_dev *bd = data;
	bd->ms_adsp_log_fetch_time_interval = (u16) val;
	cancel_delayed_work_sync(&bd->ms_adsp_log_fetch_work);
	queue_delayed_work(bd->ms_adsp_log_fetch_wq, &bd->ms_adsp_log_fetch_work, 0);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(ms_adsp_log_fetch_time_interval_ops, ms_adsp_log_fetch_time_interval_read,
			ms_adsp_log_fetch_time_interval_write, "%d\n");

// MSCHANGE end
static void battery_dbg_add_debugfs(struct battery_dbg_dev *bd)
{
	int rc;
	struct dentry *bd_dir, *file;	// MSCHANGE

	bd_dir = debugfs_create_dir("battery_debug", NULL);
	if (IS_ERR(bd_dir)) {
		rc = PTR_ERR(bd_dir);
		pr_err("Failed to create battery debugfs directory: %d\n", rc);
		return;
	}
//MSCHANGE Start
	file = debugfs_create_file("log_categories", 0600, bd_dir, bd, &log_categories_ops);
	if (IS_ERR(file)) {
		rc = PTR_ERR(file);
		pr_err("Failed to create log_categories file, rc=%d\n", rc);
		goto error;
	}

	file = debugfs_create_file("log_fetch_time_interval", 0600, bd_dir, bd, &ms_adsp_log_fetch_time_interval_ops);
	if (IS_ERR(file)) {
		rc = PTR_ERR(file);
		pr_err("Failed to create log_fetch_time_interval file, rc=%d\n",
			rc);
		goto error;
	}
// MSCHANGE end

	rc = battery_dbg_create_votables(bd, bd_dir);
	if (rc) {
		pr_err("Failed to create votables: %d\n", rc);
		goto error;
	}

	bd->debugfs_dir = bd_dir;

	return;
error:
	debugfs_remove_recursive(bd_dir);
	return;
}
#else
static void battery_dbg_add_debugfs(struct battery_dbg_dev *bd)
{
	return;
}
#endif

static ssize_t qbg_blob_write(struct file *filp, struct kobject *kobj,
				    struct bin_attribute *attr, char *buf,
				    loff_t pos, size_t count)
{
	int rc;
	struct device *dev = kobj_to_dev(kobj);
	struct battery_dbg_dev *bd = dev_get_drvdata(dev);

	rc = get_qbg_context_write(bd, 0); /* second arg is ignored */
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t qbg_blob_read(struct file *filp, struct kobject *kobj,
				      struct bin_attribute *attr, char *buf,
				      loff_t pos, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct battery_dbg_dev *bd = dev_get_drvdata(dev);

	return memory_read_from_buffer(buf, count, &pos, bd->qbg_blob.data,
				       bd->qbg_blob.size);
}

static struct bin_attribute qbg_blob = {
	.attr = {
		.name = "qbg_context",
		.mode = 0600,
	},
	.read = qbg_blob_read,
	.write = qbg_blob_write,
};

static int battery_dbg_add_dev_attr(struct battery_dbg_dev *bd)
{
	int rc;

	bd->qbg_blob.data = bd->qbg_dump.buf;
	bd->qbg_blob.size = 0;

	rc = device_create_bin_file(bd->dev, &qbg_blob);
	if (rc)
		dev_err(bd->dev, "Failed to create qbg_context bin file: %d\n",
				rc);

	return rc;
}

static int battery_dbg_probe(struct platform_device *pdev)
{
	struct battery_dbg_dev *bd;
	struct pmic_glink_client_data client_data = { };
	int rc;

	bd = devm_kzalloc(&pdev->dev, sizeof(*bd), GFP_KERNEL);
	if (!bd)
		return -ENOMEM;

	bd->dev = &pdev->dev;
	client_data.id = MSG_OWNER_BD;
	client_data.name = "battery_debug";
	client_data.msg_cb = battery_dbg_callback;
	client_data.priv = bd;

	bd->client = pmic_glink_register_client(bd->dev, &client_data);
	if (IS_ERR(bd->client)) {
		rc = PTR_ERR(bd->client);
		if (rc != -EPROBE_DEFER)
			dev_err(bd->dev, "Error in registering with pmic_glink %d\n",
				rc);
		return rc;
	}

	mutex_init(&bd->lock);
	init_completion(&bd->ack);
	platform_set_drvdata(pdev, bd);
	rc = battery_dbg_add_dev_attr(bd);
	if (rc < 0)
		goto out;

// MSCHANGE start
	INIT_DELAYED_WORK(&bd->ms_adsp_log_fetch_work, ms_adsp_log_fetch_work_function);
	bd->ms_adsp_log_fetch_wq = create_workqueue("adsp_log_fetch_wq");
	if (bd->ms_adsp_log_fetch_wq == NULL) {
		dev_err(bd->dev, "Error in creating adsp_log_fetch_wq \n");
		rc = -ENOMEM;
		goto out;
	}
	bd->ms_adsp_log_fetch_time_interval = MS_DEFAULT_ADSP_LOG_FETCH_TIME_INTERVAL;
	queue_delayed_work(bd->ms_adsp_log_fetch_wq, &bd->ms_adsp_log_fetch_work, 0);
// MSCHANGE end
	battery_dbg_add_debugfs(bd);
	write_log_categories(bd,ChargerPDLoggingCategories); //MSCHANGE
	return 0;
out:
	pmic_glink_unregister_client(bd->client);
	return rc;
}

static int battery_dbg_remove(struct platform_device *pdev)
{
	struct battery_dbg_dev *bd = platform_get_drvdata(pdev);
	int rc;

	debugfs_remove_recursive(bd->debugfs_dir);
// MSCHANGE start
	cancel_delayed_work_sync(&bd->ms_adsp_log_fetch_work);
	destroy_workqueue(bd->ms_adsp_log_fetch_wq);
// MSCHANGE end
	rc = pmic_glink_unregister_client(bd->client);
	if (rc < 0) {
		pr_err("Error unregistering from pmic_glink, rc=%d\n", rc);
		return rc;
	}

	return 0;
}

static const struct of_device_id battery_dbg_match_table[] = {
	{ .compatible = "qcom,battery-debug" },
	{},
};

static struct platform_driver battery_dbg_driver = {
	.driver	= {
		.name = "qti_battery_debug",
		.of_match_table = battery_dbg_match_table,
	},
	.probe	= battery_dbg_probe,
	.remove	= battery_dbg_remove,
};
module_platform_driver(battery_dbg_driver);

MODULE_DESCRIPTION("QTI Glink battery debug driver");
MODULE_LICENSE("GPL v2");
