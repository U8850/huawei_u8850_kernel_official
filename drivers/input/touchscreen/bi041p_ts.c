#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <mach/gpio.h>
#include <mach/vreg.h>
#include <linux/bi041p_ts.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/wakelock.h>
#include <linux/proc_fs.h>
#include "../../../arch/arm/mach-msm/smd_private.h"
#include "../../../arch/arm/mach-msm/proc_comm.h"

#ifdef CONFIG_FIH_TOUCHSCREEN_INNOLUX
extern int innolux_ts_active;
#endif
extern int bu21018mwv_active;
extern int i2c_elan_touch_update;
extern int bq275x0_battery_elan_update_mode;

// flag of HW type
static int hw_ver = HW_UNKNOW;
static int bi041p_debug = 0;
static bool bIsPenUp = 0;
static int fw_delay = 100;
static int update_mode = 0;
static int noise_time = 150;
#ifdef CONFIG_FIH_FTM
static int file_path = 0;
static int ftm_fw_update_result = 0;
#endif

bool bBackCapKeyPressed = 0;
bool bMenuCapKeyPressed = 0;
bool bHomeCapKeyPressed = 0;
bool bSearchCapKeyPressed = 0;

static struct i2c_client *fclient;
static int int_disable = 0;

// for FQC firmware version
static int msm_cap_touch_fw_version;
static int ts_hw_ver = 0;

module_param_named(
    fw_version, msm_cap_touch_fw_version, int, S_IRUGO | S_IWUSR | S_IWGRP
);

static struct workqueue_struct *fwupdate_wq;
static struct workqueue_struct *resume_wq;

struct bi041p_info {
    struct i2c_client *client;
    struct input_dev *input;
    struct work_struct wqueue;
    struct work_struct wqueue_f;
    struct work_struct wqueue_r;
    struct mutex mutex;
#ifdef CONFIG_HAS_EARLYSUSPEND
    struct early_suspend es;
#endif
    struct wake_lock wake_lock;
} bi041p;

#ifdef CONFIG_HAS_EARLYSUSPEND
static void bi041p_early_suspend(struct early_suspend *h);
static void bi041p_late_resume(struct early_suspend *h);
#endif

static int bi041p_i2c_recv(u8 *buf, u32 count)
{
	struct i2c_msg msgs[] = {
		[0] = {
			.addr = bi041p.client->addr,
			.flags = I2C_M_RD,
			.len = count,
			.buf = buf,
		},
	};
	
	return (i2c_transfer(bi041p.client->adapter, msgs, 1) < 0) ? -EIO : 0;
}

static int bi041p_i2c_send(u8 *buf, u32 count)
{
	struct i2c_msg msgs[]= {
		[0] = {
			.addr = bi041p.client->addr,
			.flags = 0,
			.len = count,
			.buf = buf,
		},
	};
	
	return (i2c_transfer(bi041p.client->adapter, msgs, 1) < 0) ? -EIO : 0;
}

#define FW_MAJOR(x) ((((int)((x)[1])) & 0xF) << 4) + ((((int)(x)[2]) & 0xF0) >> 4)
#define FW_MINOR(x) ((((int)((x)[2])) & 0xF) << 4) + ((((int)(x)[3]) & 0xF0) >> 4)

static int bi041p_get_fw_version(void)
{
	int major, minor;
	char cmd[4] = { 0x53, 0x00, 0x00, 0x01 };
	char buffer[4];

	bi041p_i2c_send(cmd, ARRAY_SIZE(cmd));
	msleep(20);
	bi041p_i2c_recv(buffer, ARRAY_SIZE(buffer));

	if (buffer[0] != 0x52) {
		return -1;
	}

	major = FW_MAJOR(buffer);
	minor = FW_MINOR(buffer);
	msm_cap_touch_fw_version = major * 100 + minor;
	
	printk(KERN_INFO "[Touch] Get ELAN firmware version %d.\n", msm_cap_touch_fw_version);

	return 0;
}

static int bi041p_get_tp_version(void)
{
	char cmd[4] = {0x53, 0xF0, 0x00, 0x01};
	char buffer[4];
	int ret = -1;
	
	bi041p_i2c_send(cmd, ARRAY_SIZE(cmd));
	msleep(20);
	bi041p_i2c_recv(buffer, ARRAY_SIZE(buffer));
	
	if (buffer[0] != 0x52) {
		return ret;
	}
	
	if (buffer[3] == 0x11)
	{
		printk(KERN_INFO "[Touch] Touch is WINTEK 0x%2x 0x%2x 0x%2x 0x%2x.\n", buffer[0], buffer[1], buffer[2], buffer[3]);
		ts_hw_ver = 1;
		ret = 1;
	}
	else
	{
		printk(KERN_INFO "[Touch] Touch is ELAN 0x%2x 0x%2x 0x%2x 0x%2x.\n", buffer[0], buffer[1], buffer[2], buffer[3]);
		ts_hw_ver = 2;
		ret = 2;
	}
	
	return ret;
}

#define XCORD1(x) ((((int)((x)[1]) & 0xF0) << 4) + ((int)((x)[2])))
#define YCORD1(y) ((((int)((y)[1]) & 0x0F) << 8) + ((int)((y)[3])))
#define XCORD2(x) ((((int)((x)[4]) & 0xF0) << 4) + ((int)((x)[5])))
#define YCORD2(y) ((((int)((y)[4]) & 0x0F) << 8) + ((int)((y)[6])))

static void bi041p_isr_workqueue(struct work_struct *work)
{
	u8 buffer[9];
	int cnt, virtual_button;
	int retry = 3;
	
	do
	{
		if (bi041p_i2c_recv(buffer, ARRAY_SIZE(buffer)) == 0)
			break;
		
		printk(KERN_INFO "[Touch] %s: retry = %d\n", __func__, 4-retry);
	} while (retry--);
		
	
	if (bi041p_debug)
	    printk(KERN_INFO "[Touch] %s: buffer[0]=0x%.2x,buffer[1]=0x%.2x,buffer[2]=0x%.2x,buffer[3]=0x%.2x,buffer[4]=0x%.2x,buffer[5]=0x%.2x,buffer[6]=0x%.2x,buffer[7]=0x%.2x,buffer[8]=0x%.2x\n", __func__, buffer[0],buffer[1],buffer[2],buffer[3],buffer[4],buffer[5],buffer[6],buffer[7],buffer[8]);
	
	if (buffer[0] == 0x5A)
	{
		cnt = (buffer[8] ^ 0x01) >> 1;
		virtual_button = (buffer[8]) >> 3;
		
		if ((virtual_button == 0) && !bBackCapKeyPressed && !bMenuCapKeyPressed && !bHomeCapKeyPressed && !bSearchCapKeyPressed)
		{
#ifdef CONFIG_FIH_FTM
			if (cnt) {
                input_report_abs(bi041p.input, ABS_X, XCORD1(buffer));
                input_report_abs(bi041p.input, ABS_Y, abs(1088 - YCORD1(buffer)));
                input_report_abs(bi041p.input, ABS_PRESSURE, 255);
                input_report_key(bi041p.input, BTN_TOUCH, 1);
            } else {
                input_report_abs(bi041p.input, ABS_PRESSURE, 0);
                input_report_key(bi041p.input, BTN_TOUCH, 0);
            }
#else
			if (cnt) {
				input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 255);
				input_report_abs(bi041p.input, ABS_MT_POSITION_X, XCORD1(buffer));
				input_report_abs(bi041p.input, ABS_MT_POSITION_Y, abs(1088 - YCORD1(buffer)));
				input_mt_sync(bi041p.input);
			} else {
				input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 0);
				input_report_abs(bi041p.input, ABS_MT_POSITION_X, XCORD1(buffer));
				input_report_abs(bi041p.input, ABS_MT_POSITION_Y, abs(1088 - YCORD1(buffer)));
				input_mt_sync(bi041p.input);
			}
			if (cnt > 1) {
				input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 255);
				input_report_abs(bi041p.input, ABS_MT_POSITION_X, XCORD2(buffer));
				input_report_abs(bi041p.input, ABS_MT_POSITION_Y, abs(1088 - YCORD2(buffer)));
				input_mt_sync(bi041p.input);
			} else {
				input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 0);
				input_report_abs(bi041p.input, ABS_MT_POSITION_X, XCORD2(buffer));
				input_report_abs(bi041p.input, ABS_MT_POSITION_Y, abs(1088 - YCORD2(buffer)));
				input_mt_sync(bi041p.input);
			}
#endif
            if (!cnt)
            {
				if (bi041p_debug)
					printk(KERN_INFO "[Touch] %s: Pen Up.\n", __func__);
					
                bIsPenUp = 1;
			}
            else
            {
				if (bi041p_debug)
					printk(KERN_INFO "[Touch] %s: Pen Down.\n", __func__);
					
                bIsPenUp = 0;
			}
		}
		else if ((virtual_button == 0) && (bBackCapKeyPressed || bMenuCapKeyPressed || bHomeCapKeyPressed || bSearchCapKeyPressed))
		{
			if (bBackCapKeyPressed)
			{
				input_report_key(bi041p.input, KEY_BACK, 0);
				bBackCapKeyPressed = 0;
				bMenuCapKeyPressed = 0;
				bHomeCapKeyPressed = 0;
				bSearchCapKeyPressed = 0;
			}
			else if (bMenuCapKeyPressed)
			{
				input_report_key(bi041p.input, KEY_MENU, 0);
				bBackCapKeyPressed = 0;
				bMenuCapKeyPressed = 0;
				bHomeCapKeyPressed = 0;
				bSearchCapKeyPressed = 0;
			}
			else if (bHomeCapKeyPressed)
			{
				input_report_key(bi041p.input, KEY_HOME, 0);
				bBackCapKeyPressed = 0;
				bMenuCapKeyPressed = 0;
				bHomeCapKeyPressed = 0;
				bSearchCapKeyPressed = 0;
			}
			else if (bSearchCapKeyPressed)
			{
				input_report_key(bi041p.input, KEY_SEARCH, 0);
				bBackCapKeyPressed = 0;
				bMenuCapKeyPressed = 0;
				bHomeCapKeyPressed = 0;
				bSearchCapKeyPressed = 0;
			}
		}
		else if (virtual_button == 2 && !bBackCapKeyPressed)
		{
            if (!bIsPenUp)
            {
				input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 0);
				input_mt_sync(bi041p.input);
                bIsPenUp = 1;
            }
            
			if (hw_ver == HW_FD1_PR3 || hw_ver == HW_FD1_PR4)
			{
				input_report_key(bi041p.input, KEY_MENU, 1);
				bMenuCapKeyPressed = 1;
			}
			else
			{
				input_report_key(bi041p.input, KEY_BACK, 1);
				bBackCapKeyPressed = 1;
			}
		}
		else if (virtual_button == 4 && !bMenuCapKeyPressed)
		{
            if (!bIsPenUp)
            {
				input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 0);
				input_mt_sync(bi041p.input);
                bIsPenUp = 1;
            }
            
			if (hw_ver == HW_FD1_PR3 || hw_ver == HW_FD1_PR4)
			{
				input_report_key(bi041p.input, KEY_HOME, 1);
				bHomeCapKeyPressed = 1;
			}
			else
			{
				input_report_key(bi041p.input, KEY_MENU, 1);
				bMenuCapKeyPressed = 1;
			}
		}
		else if (virtual_button == 8 && !bHomeCapKeyPressed)
		{
            if (!bIsPenUp)
            {
				input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 0);
				input_mt_sync(bi041p.input);
                bIsPenUp = 1;
            }
            
			if (hw_ver == HW_FD1_PR3)
			{
				input_report_key(bi041p.input, KEY_SEARCH, 1);
				bSearchCapKeyPressed = 1;
			}
			else if (hw_ver == HW_FD1_PR4)
			{
				input_report_key(bi041p.input, KEY_BACK, 1);
				bBackCapKeyPressed = 1;
			}
			else
			{
				input_report_key(bi041p.input, KEY_HOME, 1);
				bHomeCapKeyPressed = 1;
			}
		}
		else if (virtual_button == 16 && !bSearchCapKeyPressed)
		{
            if (!bIsPenUp)
            {
				input_report_abs(bi041p.input, ABS_MT_TOUCH_MAJOR, 0);
				input_mt_sync(bi041p.input);
                bIsPenUp = 1;
            }
            
			if (hw_ver == HW_FD1_PR3)
			{
				input_report_key(bi041p.input, KEY_BACK, 1);
				bBackCapKeyPressed = 1;
			}
			else if (hw_ver == HW_FD1_PR4)
			{
				input_report_key(bi041p.input, KEY_SEARCH, 1);
				bSearchCapKeyPressed = 1;
			}
			else
			{
				input_report_key(bi041p.input, KEY_SEARCH, 1);
				bSearchCapKeyPressed = 1;
			}
		}
		
		input_sync(bi041p.input);
	}
	else if ((buffer[0] == 0x55) && (buffer[1] == 0x55) && (buffer[2] == 0x55) && (buffer[3] == 0x55))
	{
		printk(KERN_INFO "[Touch] %s: Receive the hello packet!\n", __func__);
	}
	
	enable_irq(bi041p.client->irq);
}

static irqreturn_t bi041p_isr(int irq, void * handle)
{
    struct bi041p_info *ts = handle;
	
	disable_irq_nosync(bi041p.client->irq);
	schedule_work(&ts->wqueue);
	
    return IRQ_HANDLED;
}

static int bi041p_set_power_domain(void)
{
	struct vreg *vreg_ldo12;
	int rc;
	
	vreg_ldo12 = vreg_get(NULL, "gp9");
	
	if (IS_ERR(vreg_ldo12))
	{
		printk(KERN_ERR "%s: gp9 vreg get failed (%ld)\n", __func__, PTR_ERR(vreg_ldo12));
		return 0;
	}
	
	rc = vreg_set_level(vreg_ldo12, 3000);
	
	if (rc)
	{
		printk(KERN_ERR "%s: vreg LDO12 set level failed (%d)\n", __func__, rc);
		return 0;
	}
	
	rc = vreg_enable(vreg_ldo12);
	
	if (rc)
	{
		printk(KERN_ERR "%s: LDO12 vreg enable failed (%d)\n", __func__, rc);
		return 0;
	}
	
	return 1;
}

static int bi041p_reset(void)
{
	int retry = 100,		/* retry count of device detecting */	    
		pkt;				/* packet buffer */
	
	if (gpio_tlmm_config(GPIO_CFG(56, 0, GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), GPIO_CFG_ENABLE))
	{
		printk(KERN_ERR "[Touch] %s: gpio_tlmm_config: 56 failed.\n", __func__);
		return 0;
	}
	else
	{
		gpio_set_value(56, 1);
		msleep(1);
		gpio_set_value(56, 0);
	}
	
	do
	{
	    if (gpio_get_value(42) == 0)
        {
			bi041p_i2c_recv((char *)&pkt, sizeof(int));
			pkt ^= 0x55555555;
        }
        
		msleep(10);
		
		if (--retry == 0)
		{
			printk(KERN_ERR "[Touch] %s: receive hello package timeout.\n", __func__);
			return 0;
		}
	}while (pkt);
	
	return 1;
}

void bi041p_disable(int en)
{
	if (en && !int_disable)
	{
		disable_irq_nosync(bi041p.client->irq);
		int_disable = 1;
	}
	else if (!en && int_disable)
	{
		enable_irq(bi041p.client->irq);
		int_disable = 0;
	}
}

static void resume_work_func(struct work_struct *work)
{
	if (bi041p_reset())
        bi041p_disable(0);
}

#ifdef CONFIG_FIH_FTM
//#define FW_FILE_1 "/ftm/emc_ISP_cds.cds"
#define FW_FILE_FTM_1 "/ftm/elan_fw_wintek.cds"
#define FW_FILE_FTM_2 "/ftm/elan_fw_elan.cds"
//#define FW_FILE_2 "/sdcard/emc_ISP_cds.cds"
#define FW_FILE_SD_1 "/sdcard/elan_fw_wintek.cds"
#define FW_FILE_SD_2 "/sdcard/elan_fw_elan.cds"
#else
#define FW_FILE "/data/misc/emc_ISP_cds.cds"
#endif

static int fw_update(void)
{
	static struct file *filp = NULL;
	char buf[130] = {0};
	char buf1[128] = {0};
	char buf2[0x1A4] = {0};	// freq
	char buf3[5] = {0};		// freq
	char tmp = 0;			// freq
	int nRead = 0;
	int i = 0;
	int wfail = 0;
	
	char cmd_writepassword[3] = {0x00, 0x9C, 0xA5};
	char cmd_erasecodeoption[1] = {0x08};
	char cmd_masserase[1] = {0x04};
	char cmd_writecodeoption[5] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF};
	char cmd_read[5] = {0x05, 0x00};
	char cmd_resetmcu[1] = {0x09};
	
#ifdef CONFIG_FIH_FTM
	if (file_path == 1)
	{
		if (bi041p_get_tp_version() == 1)
			filp = filp_open(FW_FILE_FTM_1, O_RDONLY, 0);
		else if (bi041p_get_tp_version() == 2)
			filp = filp_open(FW_FILE_FTM_2, O_RDONLY, 0);
		else
			return -1;
	}
	else if (file_path == 2)
	{
		if (bi041p_get_tp_version() == 1)
			filp = filp_open(FW_FILE_SD_1, O_RDONLY, 0);
		else if (bi041p_get_tp_version() == 2)
			filp = filp_open(FW_FILE_SD_2, O_RDONLY, 0);
		else
			return -1;
	}
	else
	{
		printk(KERN_ERR "[Touch] %s: Invalid file path!", __func__);
		ftm_fw_update_result = -1;
		return -1;
	}
#else
	filp = filp_open(FW_FILE, O_RDONLY, 0);
#endif
	
    if (IS_ERR(filp))
    {
        printk(KERN_ERR "[Touch] %s: Open file failed!\n", __func__);
        filp = NULL;
#ifdef CONFIG_FIH_FTM
		ftm_fw_update_result = -1;
#endif
        return -1;
    }
    else
		printk(KERN_INFO "[Touch] %s: Start ELAN touch update.\n", __func__);
		
    // Start update
	wake_lock(&bi041p.wake_lock);
	i2c_elan_touch_update = 1;
	update_mode = 1;
	bq275x0_battery_elan_update_mode = 1;
	
    if (i2c_master_send(fclient, cmd_writepassword, 3) != 3)
    {
		printk(KERN_ERR "[Touch] %s: Write password failed!\n", __func__);
		wfail = 1;
		goto fu_err;
	}
	
    msleep(1);
    
    if (i2c_master_send(fclient, cmd_erasecodeoption, 1) != 1)
    {
		printk(KERN_ERR "[Touch] %s: Erase code option failed!\n", __func__);
		wfail = 1;
		goto fu_err;
	}
	
    msleep(1);
    
    if (i2c_master_send(fclient, cmd_masserase, 1) != 1)
    {
		printk(KERN_ERR "[Touch] %s: Mass erase failed!\n", __func__);
		wfail = 1;
		goto fu_err;
	}
	
    msleep(1);
    
    if (i2c_master_send(fclient, cmd_writecodeoption, 5) != 5)
    {
		printk(KERN_ERR "[Touch] %s: Write code option failed!\n", __func__);
		wfail = 1;
		goto fu_err;
	}
	
    msleep(1);
    
    //for (i=0; i<0x80; i++)
    for (i=0; i<0x100; i++)
    {
		buf[0] = 0x02;
		buf[1] = (unsigned char)i;
		
		printk(KERN_INFO "[Touch] %s: Page %d.\n", __func__, i);
#ifdef CONFIG_FIH_FTM
		ftm_fw_update_result = i;
#endif
		
		nRead = filp->f_op->read(filp, (unsigned char __user *)&buf[2], 128, &filp->f_pos);
		
		if (nRead < 128)
		{
			memset(&buf[2+nRead], 0xff, 128 - nRead);
		}
		
		if (i < 0x80)
		{
		// write page
		if (i2c_master_send(fclient, buf, 128+2) != 130)
		{
			printk(KERN_ERR "[Touch] %s: Write page failed!\n", __func__);
			wfail = 1;
			break;
		}
		
		msleep(100);
		
		// read page
		cmd_read[0] = 0x05;
		cmd_read[1] = (unsigned char)i;
		
		if (i2c_master_send(fclient, cmd_read, 2) != 2)
		{
			printk(KERN_ERR "cmd_read error\n");
			wfail = 1;
			break;
		}
		
		msleep(1);
		
		if (i2c_master_recv(fclient, buf1, 128) != 128)
		{
			printk(KERN_ERR "[Touch] %s: Read page failed!\n", __func__);
			wfail = 1;
			break;
		}
		
		// verify
		if (memcmp(&buf[2], buf1, 128) != 0)
		{
			printk(KERN_ERR "[Touch] %s: Compare failed!\n", __func__);
			wfail = 1;
			break;
		}
			
		if (fw_delay > 0)
			msleep(fw_delay);
		}
	}
	
	// for freq
	nRead = filp->f_op->read(filp, (unsigned char __user *)&buf2[0], 0x1A4, &filp->f_pos);
	
	if (nRead == 0x1A4)
	{
		buf3[0] = 0x01;
		
		//
		buf3[1] = 0xE0;
		// bit 28
		tmp = (buf2[0x1A3] & 0x01) << 4;
		buf3[1] |= tmp;
		// bit 27
		tmp = (buf2[0x195] & 0x01) << 3;
		buf3[1] |= tmp;
		// bit 26
		tmp = (buf2[0x186] & 0x01) << 2;
		buf3[1] |= tmp;
		// bit 25
		tmp = (buf2[0x178] & 0x01) << 1;
		buf3[1] |= tmp;
		// bit 24
		tmp = (buf2[0x16A] & 0x01);
		buf3[1] |= tmp;
		
		//
		buf3[2] = 0x0;
		// bit 23
		tmp = (buf2[0x15C] & 0x01) << 7;
		buf3[2] |= tmp;
		// bit 22
		tmp = (buf2[0x14E] & 0x01) << 6;
		buf3[2] |= tmp;
		// bit 21
		tmp = (buf2[0x140] & 0x01) << 5;
		buf3[2] |= tmp;
		// bit 20
		tmp = (buf2[0x132] & 0x01) << 4;
		buf3[2] |= tmp;
		// bit 19
		tmp = (buf2[0x124] & 0x01) << 3;
		buf3[2] |= tmp;
		// bit 18
		tmp = (buf2[0x116] & 0x01) << 2;
		buf3[2] |= tmp;
		// bit 17
		tmp = (buf2[0x108] & 0x01) << 1;
		buf3[2] |= tmp;
		// bit 16
		tmp = (buf2[0xF8] & 0x01);
		buf3[2] |= tmp;
		
		//
		buf3[3] = 0x0;
		// bit 15
		tmp = (buf2[0xE8] & 0x01) << 7;
		buf3[3] |= tmp;
		// bit 14
		tmp = (buf2[0xDA] & 0x01) << 6;
		buf3[3] |= tmp;
		// bit 13
		tmp = (buf2[0xCC] & 0x01) << 5;
		buf3[3] |= tmp;
		// bit 12
		tmp = (buf2[0xBE] & 0x01) << 4;
		buf3[3] |= tmp;
		// bit 11
		tmp = (buf2[0xB0] & 0x01) << 3;
		buf3[3] |= tmp;
		// bit 10
		tmp = (buf2[0xA2] & 0x01) << 2;
		buf3[3] |= tmp;
		// bit 9
		tmp = (buf2[0x94] & 0x01) << 1;
		buf3[3] |= tmp;
		// bit 8
		tmp = (buf2[0x87] & 0x01);
		buf3[3] |= tmp;
		
		//
		buf3[4] = 0x0;
		// bit 7
		tmp = (buf2[0x7A] & 0x01) << 7;
		buf3[4] |= tmp;
		// bit 6
		tmp = (buf2[0x6D] & 0x01) << 6;
		buf3[4] |= tmp;
		// bit 5
		tmp = (buf2[0x60] & 0x01) << 5;
		buf3[4] |= tmp;
		// bit 4
		tmp = (buf2[0x53] & 0x01) << 4;
		buf3[4] |= tmp;
		// bit 3
		tmp = (buf2[0x46] & 0x01) << 3;
		buf3[4] |= tmp;
		// bit 2
		tmp = (buf2[0x39] & 0x01) << 2;
		buf3[4] |= tmp;
		// bit 1
		tmp = (buf2[0x2C] & 0x01) << 1;
		buf3[4] |= tmp;
		// bit 0
		tmp = (buf2[0x1F] & 0x01);
		buf3[4] |= tmp;
		
		printk("buf3[0] = 0x%x\nbuf3[1] = 0x%x\nbuf3[2] = 0x%x\nbuf3[3] = 0x%x\nbuf3[4] = 0x%x", buf3[0], buf3[1], buf3[2], buf3[3], buf3[4]);
		
		if (i2c_master_send(fclient, buf3, 5) != 5)
		{
			printk(KERN_ERR "[Touch] %s: Write freq failed!\n", __func__);
			wfail = 1;
		}
	}
	else
		printk(KERN_ERR "[Touch] %s: Can't read freq data!\n", __func__);
	
    if (i2c_master_send(fclient, cmd_resetmcu, 1) != 1)
    {
		printk(KERN_ERR "[Touch] %s: Reset MCU failed!\n", __func__);
		wfail = 1;
	}
	
fu_err:
	
	// stop update
	wake_unlock(&bi041p.wake_lock);
	i2c_elan_touch_update = 0;
	update_mode = 0;
	bq275x0_battery_elan_update_mode = 0;

	vfs_fsync(filp, filp->f_path.dentry, 1);
	filp_close(filp, NULL);
	filp = NULL;
	
	if (!wfail)
		return 1;	// success
	else
		return 0;	// fail
}

static void fwupdate_work_func(struct work_struct *work)
{
	int i;
	int ret;
	
	bi041p_disable(1);
	
	for (i=0; i<20; i++)
	{
		ret = fw_update();
		
		if (ret == 1)
		{
			printk(KERN_INFO "[Touch] %s: Firmware update success.\n", __func__);
#ifdef CONFIG_FIH_FTM
//			ftm_fw_update_result = 1;
#else
			proc_comm_ftm_hw_reset();
#endif
			break;
		}
		else if (ret < 0)
		{
			printk(KERN_ERR "[Touch] %s: Not I2C unstable issue. STOP retry!\n", __func__);
			return;
		}
		else
		{
			printk(KERN_INFO "[Touch] %s: Retry = %d. ELAN reset...\n", __func__, i+1);
			
			if (gpio_tlmm_config(GPIO_CFG(56, 0, GPIO_CFG_OUTPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), GPIO_CFG_ENABLE))
			{
				printk(KERN_ERR "[Touch] %s: gpio_tlmm_config: 56 failed.\n", __func__);
				return;
			}
			else
			{
				gpio_set_value(56, 1);
				msleep(1);
				gpio_set_value(56, 0);
				msleep(5000);
			}
		}
	}
}

//#define COUNT_TEN_SEC 150
#define MAX_X_AXIS 11

static int bi041p_noise_monitor(void)
{
	int i=0, j=0;
	u8 buffer[12];
	u8 max_adc[11];
	int avg_adc = 0;
	
	char cmd1[4] = {0x55,0x55,0x55,0x55};
	char cmd2[4] = {0x54,0x9d,0x01,0x01};
	char cmd3[4] = {0x57,0x31,0xff,0xff};
	char cmd4[4] = {0xa5,0xa5,0xa5,0xa5};
	
	disable_irq_nosync(bi041p.client->irq);
	
	// enter test mode
	bi041p_i2c_send(cmd1, ARRAY_SIZE(cmd1));
	msleep(1);
	
	// switch ADC mode
	bi041p_i2c_send(cmd2, ARRAY_SIZE(cmd2));
	msleep(100);
	
	for (j=0;j<MAX_X_AXIS;j++)
		max_adc[j] = 0;
	
	// read X axis ADC value
	for (i=0;i<noise_time;i++)
	{
		if (bi041p_i2c_send(cmd3, ARRAY_SIZE(cmd3)) < 0)
			printk(KERN_ERR "[Touch] %s: bi041p_i2c_send() failed!", __func__);
		
		msleep(1);
		
		if (bi041p_i2c_recv(buffer, ARRAY_SIZE(buffer)) < 0)
			printk(KERN_ERR "[Touch] %s: bi041p_i2c_recv() failed!", __func__);
		
		for (j=0;j<MAX_X_AXIS;j++)
		{
			if (buffer[j] > max_adc[j])
				max_adc[j] = buffer[j];
		}
		
		msleep(30);
		
		printk(KERN_INFO "buffer  %3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d", buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7], buffer[8], buffer[9], buffer[10], buffer[11]);
		printk(KERN_INFO "max_adc %3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d", max_adc[0], max_adc[1], max_adc[2], max_adc[3], max_adc[4], max_adc[5], max_adc[6], max_adc[7], max_adc[8], max_adc[9], max_adc[10]);
	}
	
	for (j=0;j<MAX_X_AXIS;j++)
		avg_adc += max_adc[j];
	
	avg_adc = avg_adc / MAX_X_AXIS;
	
	printk(KERN_INFO "avg_adc = %d\n", avg_adc);
	
	// exit test mode
	bi041p_i2c_send(cmd4, ARRAY_SIZE(cmd4));
	msleep(1);
	
	enable_irq(bi041p.client->irq);
	
	return avg_adc;
}

static int bi041p_misc_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
    void __user *argp = (void __user *)arg;
    int value;
    
	switch (cmd)
	{
		case 65000:
		    printk(KERN_INFO "[Touch] Debug message ON.\n");
			bi041p_debug = 1;
			break;
		case 65001:
		    printk(KERN_INFO "[Touch] Debug message OFF.\n");
			bi041p_debug = 0;
			break;
		case 65002:
			bi041p_get_fw_version();
			return msm_cap_touch_fw_version;
			break;
		case 65003:
			queue_work(fwupdate_wq, &bi041p.wqueue_f);
			break;
		case 65004:
            if (copy_from_user(&value, argp, sizeof(value)))
                value = (unsigned int)arg;
            
            fw_delay = value;
            
            printk(KERN_INFO "[Touch] Set firmware update delay value = %d\n", fw_delay);
            break;
#ifdef CONFIG_FIH_FTM
		case 65005:
			file_path = 1;
			queue_work(fwupdate_wq, &bi041p.wqueue_f);
			break;
		case 65006:
			file_path = 2;
			queue_work(fwupdate_wq, &bi041p.wqueue_f);
			break;
		case 65007:
			return ftm_fw_update_result;
			break;
#endif
		case 65008:
			return bi041p_get_tp_version();
			break;
		case 65009:
            if (copy_from_user(&value, argp, sizeof(value)))
                value = (unsigned int)arg;
			
			noise_time = value;
			
			if (noise_time > 500)
				noise_time = 500;
			
			printk(KERN_INFO "[Touch] %s: noise test times = %d\n", __func__, noise_time);
			
			return bi041p_noise_monitor();
			break;
		default:
			printk(KERN_ERR "[Touch] %s: unknow IOCTL.\n", __func__);
			break;
	}
	
	return 0;
}

static struct file_operations bi041p_misc_fops = {
	.owner =   THIS_MODULE,
	.ioctl =   bi041p_misc_ioctl,
};

static struct miscdevice bi041p_misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "bi041p",
	.fops = &bi041p_misc_fops,
};

static int bi041p_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct i2c_adapter *fw_adap = i2c_get_adapter(0);
	
#ifdef CONFIG_FIH_TOUCHSCREEN_INNOLUX
	// check INNOLUX exist
	if (innolux_ts_active)
	{
		printk(KERN_INFO "[Touch] %s: INNOLUX already exists. bi041p_probe() abort.\n", __func__);
		return -ENODEV;
	}
#endif
	
	// check ROHM exist
	if (bu21018mwv_active)
	{
		printk(KERN_INFO "[Touch] %s: BU21018MWV already exists. bi041p_probe() abort.\n", __func__);
		return -ENODEV;
	}
		
	// read HWID
	if ((fih_get_product_id() == Product_FD1 && fih_get_product_phase() == Product_PR3) ||
		(fih_get_product_id() == Product_FD1 && fih_get_product_phase() == Product_PR231))
	{
		hw_ver = HW_FD1_PR3;
		printk(KERN_INFO "[Touch] Virtual key mapping for FD1 PR235.\n");
	}
	else if ((fih_get_product_id() == Product_FD1 && fih_get_product_phase() == Product_PR4) ||
			(fih_get_product_id() == Product_FD1 && fih_get_product_phase() == Product_PR5))
	{
	    hw_ver = HW_FD1_PR4;
	    printk(KERN_INFO "[Touch] Virtual key mapping for FD1 PR4 and PR5.\n");
	}
	else if (fih_get_product_id() == Product_FD1)
	{
	    hw_ver = HW_FD1_PR4;
	    printk(KERN_INFO "[Touch] Virtual key mapping for FD1.\n");
	}
	else
	{
		hw_ver = HW_FB0;
		printk(KERN_INFO "[Touch] Virtual key mapping for FB0.\n");
	}
	
	// check I2C
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
	{
		printk(KERN_ERR "[Touch] %s: Check I2C functionality failed!\n", __func__);
		return -ENODEV;
	}
	
    //
	bi041p.client = client;
	strlcpy(client->name, "bi041p", I2C_NAME_SIZE);
	i2c_set_clientdata(client, &bi041p);
    
	// set power
	if (!bi041p_set_power_domain())
	{
		printk(KERN_ERR "[Touch] %s: Set power fail!\n", __func__);
    	return -EIO;
	}
    
    // chip reset
    if (!bi041p_reset())
    {
    	printk(KERN_ERR "[Touch] %s: Chip reset fail!\n", __func__);
		goto err_chk;
    }
    
	//
	bi041p.input = input_allocate_device();
	
    if (bi041p.input == NULL)
	{
		printk(KERN_ERR "[Touch] %s: Can not allocate memory for touch input device!\n", __func__);
		goto err1;
	}
	
	bi041p.input->name  = "bi041p";
	bi041p.input->phys  = "bi041p/input0";
	set_bit(EV_KEY, bi041p.input->evbit);
	set_bit(EV_ABS, bi041p.input->evbit);
	set_bit(EV_SYN, bi041p.input->evbit);
	set_bit(BTN_TOUCH, bi041p.input->keybit);
    set_bit(KEY_BACK, bi041p.input->keybit);
	set_bit(KEY_MENU, bi041p.input->keybit);
    set_bit(KEY_HOME, bi041p.input->keybit);
    set_bit(KEY_SEARCH, bi041p.input->keybit);

#ifdef CONFIG_FIH_FTM
	input_set_abs_params(bi041p.input, ABS_X, TS_MIN_X, TS_MAX_X, 0, 0);
	input_set_abs_params(bi041p.input, ABS_Y, TS_MIN_Y, TS_MAX_Y, 0, 0);
	input_set_abs_params(bi041p.input, ABS_PRESSURE, 0, 255, 0, 0);
#else
    input_set_abs_params(bi041p.input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(bi041p.input, ABS_MT_POSITION_X, TS_MIN_X, TS_MAX_X, 0, 0);
    input_set_abs_params(bi041p.input, ABS_MT_POSITION_Y, TS_MIN_Y, TS_MAX_Y, 0, 0);
#endif
	
	if (input_register_device(bi041p.input))
	{
		printk(KERN_ERR "[Touch] %s: Can not register touch input device.\n", __func__);
		goto err2;
	}
    
	// initial IRQ
    gpio_tlmm_config(GPIO_CFG(42, 0, GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
    
	INIT_WORK(&bi041p.wqueue, bi041p_isr_workqueue);
    
    if (request_irq(bi041p.client->irq, bi041p_isr, IRQF_TRIGGER_FALLING, "bi041p", &bi041p))
    {
        printk(KERN_ERR "[Touch] %s: Request IRQ failed.\n", __func__);
        goto err3;
    }
    
#ifdef CONFIG_HAS_EARLYSUSPEND
	bi041p.es.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 20;
	bi041p.es.suspend = bi041p_early_suspend;
	bi041p.es.resume = bi041p_late_resume;
	register_early_suspend(&bi041p.es);
#endif

	// get firmware version
	bi041p_get_fw_version();
	bi041p_get_tp_version();

	// workqueue for resume
	resume_wq = create_singlethread_workqueue("resume_wq");
	
	if (!resume_wq)
	{
		printk(KERN_ERR "[Touch] %s: Can not create resume_wq!\n", __func__);
		goto err4;
	}
	
	INIT_WORK(&bi041p.wqueue_r, resume_work_func);
	mutex_init(&bi041p.mutex);
	
err_chk:

	// check I2C for firmware update
	fclient = i2c_new_dummy(fw_adap, 0x77);
	
	if (!fclient)
	{
		printk(KERN_ERR "[Touch] %s: Check I2C address 0x77 failed!\n", __func__);
		goto err5;
	}
	
	// workqueue for firmware update
	fwupdate_wq = create_singlethread_workqueue("fwupdate_wq");
	
	if (!fwupdate_wq)
	{
		printk(KERN_ERR "[Touch] %s: Can not create fwupdate_wq!\n", __func__);
		goto err6;
	}
	
	INIT_WORK(&bi041p.wqueue_f, fwupdate_work_func);
	
	wake_lock_init(&bi041p.wake_lock, WAKE_LOCK_SUSPEND, "bi041p");
	
	// register misc device
	if (misc_register(&bi041p_misc_dev))
	{
		printk(KERN_ERR "[Touch] %s: Can not register misc device!\n", __func__);
		goto err7;
	}
	
    return 0;
	
err7:
	destroy_workqueue(fwupdate_wq);
err6:
	i2c_unregister_device(fclient);
err5:
	destroy_workqueue(resume_wq);
err4:
	free_irq(bi041p.client->irq, &bi041p);
err3:
	input_unregister_device(bi041p.input);
err2:
    input_free_device(bi041p.input);
err1:
	dev_set_drvdata(&client->dev, NULL);
	
    printk(KERN_ERR "[Touch] %s: Failed.\n", __func__);
    return -1;
}

static int ts_read_proc(char *page, char **start, off_t off,
						int count, int *eof, void *data)
{
	char ver[24];
	int len;
		
	if (ts_hw_ver == 1)
		strcpy(ver, "WINTEK");
	else if (ts_hw_ver == 2)
		strcpy(ver, "ACROSENSE");
	
	len = snprintf(page, PAGE_SIZE, "%s\n", ver);
	
	if (len <= off+count) *eof = 1;
	*start = page + off;
	len -= off;
	if (len>count) len = count;
	if (len<0) len = 0;
	
	return len;
}

static int bi041p_remove(struct i2c_client * client)
{
	printk(KERN_INFO "[Touch] %s\n", __func__);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&bi041p.es);
#endif

	input_unregister_device(bi041p.input);
	misc_deregister(&bi041p_misc_dev);
	if (fclient)
		i2c_unregister_device(fclient);
    return 0;
}

static int bi041p_suspend(struct i2c_client *client, pm_message_t state)
{
    return 0;
}

static int bi041p_resume(struct i2c_client *client)
{
    return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void bi041p_early_suspend(struct early_suspend *h)
{
	char cmd[4] = {0x54, 0x50, 0x00, 0x01};

    printk(KERN_INFO "[Touch] %s\n", __func__);
    
	if (!update_mode)
	{
		mutex_lock(&bi041p.mutex);
		bi041p_disable(1);
		bi041p_i2c_send(cmd, ARRAY_SIZE(cmd));
		mutex_unlock(&bi041p.mutex);
	}
	else
		printk(KERN_INFO "[Touch] %s: firmware update mode. do nothing.\n", __func__);
}

static void bi041p_late_resume(struct early_suspend *h)
{
	if (!update_mode)
	{
		mutex_lock(&bi041p.mutex);
		queue_work(resume_wq, &bi041p.wqueue_r);
		mutex_unlock(&bi041p.mutex);
	}
	else
		printk(KERN_INFO "[Touch] %s: firmware update mode. do nothing.\n", __func__);
}
#endif

static const struct i2c_device_id bi041p_id[] = {
	{ "bi041p", 0 },
	{ }
};

static struct i2c_driver bi041p_driver = {
	.id_table	= bi041p_id,
	.probe		= bi041p_probe,
	.remove		= bi041p_remove,
	.suspend	= bi041p_suspend,
	.resume		= bi041p_resume,
	.driver = {
		.name	= "bi041p",
	},
};

static int __init bi041p_init(void)
{
	create_proc_read_entry ("ts_hw_type", 0, NULL, ts_read_proc, NULL);
	return i2c_add_driver(&bi041p_driver);
}

static void __exit bi041p_exit(void)
{
	i2c_del_driver(&bi041p_driver);
}

module_init(bi041p_init);
module_exit(bi041p_exit);

MODULE_DESCRIPTION("ELAN BI041P-T02XB01U driver");
MODULE_AUTHOR("FIH Div2 SW2 BSP");
MODULE_LICENSE("GPL");
