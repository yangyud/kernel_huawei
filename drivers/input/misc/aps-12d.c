/* drivers/input/misc/aps-12d.c
 *
 * Copyright (C) 2010 HUAWEI, Inc.
 * Author: Benjamin Gao <gaohuajiang@huawei.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <mach/gpio.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include "aps-12d.h"
/*< DTS2010092400487  lijianzhao 20100924 begin */
#include <asm/mach-types.h>
/* DTS2010092400487  lijianzhao 20100924 end >*/ 
/* < DTS2010091001474 zhangtao 20100910 begin */
#include <linux/hardware_self_adapt.h>
/* DTS2010091001474 zhangtao 20100910 end > */
#include <mach/vreg.h>
/* < DTS2010090300997 zhangtao 20100903 begin */
#include <linux/wakelock.h>
/* DTS2010090300997 zhangtao 20100903 end > */
/*< DTS2011041700393 lijianzhao 20110417 begin */
/* modify for 4125 baseline */
#include <linux/slab.h>
/* DTS2011041700393 lijianzhao 20110417 end >*/
/* < DTS2011052606009 jiaxianghong 20110527 begin */
/* <DTS2011032104626 shenjinming 20110321 begin */
#ifdef CONFIG_HUAWEI_HW_DEV_DCT
#include <linux/hw_dev_dec.h>
#endif
/* <DTS2011032104626 shenjinming 20110321 end> */
/* < BU5D05131 gaohuajiang 20100318 begin */
/* < BU5D09124 zhangtao 20100430 begin */
//#define PROXIMITY_DB
#undef PROXIMITY_DB
/*BU5D09124 zhangtao 20100430 end >*/
#ifdef PROXIMITY_DB
#define PROXIMITY_DEBUG(fmt, args...) printk(KERN_INFO fmt, ##args)
#else
#define PROXIMITY_DEBUG(fmt, args...)
#endif

#ifndef abs
#define abs(a)  ((0 < (a)) ? (a) : -(a))
#endif

/* < DTS2010102103994 zhangtao 20101112 begin */
static int aps_debug_mask;
module_param_named(aps_debug, aps_debug_mask, int,
		S_IRUGO | S_IWUSR | S_IWGRP);

#define APS_DBG(x...) do {\
	if (aps_debug_mask) \
		printk(KERN_DEBUG x);\
	} while (0)
/* DTS2010102103994 zhangtao 20101112 end > */


static struct workqueue_struct *aps_wq;

struct aps_data {
	uint16_t addr;
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct mutex  mlock;
	struct hrtimer timer;
	struct work_struct  work;	
	int (*power)(int on);
};

static struct aps_data  *this_aps_data;

extern struct input_dev *sensor_dev;

static int aps_12d_delay = APS_12D_TIMRER;     /*1s*/
static int aps_12d_timer_count = 0;


/* < DTS2010072801000 zhangtao 20100728 begin */
static int aps_first_read = 1;
/* < DTS2010090300997 zhangtao 20100903 begin */
/* use this to make sure which device is open and make a wake lcok*/
static int light_device_minor = 0;

static int proximity_device_minor = 0;
static struct wake_lock proximity_wake_lock;
/* DTS2010090300997 zhangtao 20100903 end > */
/* DTS2010072801000 zhangtao 20100728 end > */
static atomic_t l_flag;
static atomic_t p_flag;
/* < DTS2011071500961  liujinggang 20110715 begin */
/*get value of proximity and light*/
static int proximity_data_value = 0;
static int light_data_value = 0;
/* DTS2011071500961  liujinggang 20110715 end > */

/* < DTS2010072801000 zhangtao 20100728 begin */
#define LSENSOR_MAX_LEVEL 7
static uint16_t lsensor_adc_table[LSENSOR_MAX_LEVEL] = {
	/*20, 32, 48, 64, 256, 1024, 4096 */
	5, 20, 32 , 64, 256, 640, 1024
};

/* < DTS2011042705601 zhangtao 20110427 begin */
/*static uint16_t lsensor_lux_table[LSENSOR_MAX_LEVEL] = {
	10, 225, 320, 640, 1280, 2600, 10240
};*/
/* DTS2011042705601 zhangtao 20110427 end > */
/* DTS2010072801000 zhangtao 20100728 end > */

/* <DTS2010100800714 liugaofei 20101008 begin */
#define 	TOTAL_RANGE_NUM 	2	/* aps-12d has 4 types of range,but we use two range */
#define 	MAX_ADC_OUTPUT  	4096	/* adc max value */
#define		RANGE_FIX		500	/* adc */
#define 	ADJUST_GATE		5	/* 1/ADJUST_GATE */

static unsigned int range_index = 0;
static unsigned int adjust_time = 0;
static int last_event = -1;

/* < DTS2010102103994 zhangtao 20101112 begin */
static unsigned int low_threshold_value[TOTAL_RANGE_NUM] =  {400,10};
static unsigned int high_threshold_value[TOTAL_RANGE_NUM] = {450,12};
/* DTS2010102103994 zhangtao 20101112 end > */
static unsigned int err_threshold_value[TOTAL_RANGE_NUM] = {4096,50};

static unsigned int range_reg_value[TOTAL_RANGE_NUM] = { APS_12D_RANGE_SEL_ALS_1000, \
						     APS_12D_RANGE_SEL_ALS_64000 };
static unsigned int up_range_value[TOTAL_RANGE_NUM] = {0};
static unsigned int down_range_value[TOTAL_RANGE_NUM] = {0};
/* DTS2010100800714 liugaofei 20101008 end */

static inline int aps_i2c_reg_read(struct aps_data *aps , int reg)
{
	int val;

	mutex_lock(&aps->mlock);

	/* < BU5D07525 gaohuajiang 20100411 begin */
	/* First write reg, then read reg data, resolve i2c error */
	//val = i2c_smbus_read_byte_data(aps->client, reg);
	val = i2c_smbus_write_byte(aps->client, reg);
	if (val < 0)
		printk(KERN_ERR "%s: failed to write reg[%d], err=%d\n", __FUNCTION__, reg, val);

	val = i2c_smbus_read_byte(aps->client);
	if (val < 0)
		printk(KERN_ERR "%s: failed to read reg[%d], err=%d\n", __FUNCTION__, reg, val);
	/* BU5D07525 gaohuajiang 20100411 end > */

	mutex_unlock(&aps->mlock);

	return val;
}
static inline int aps_i2c_reg_write(struct aps_data *aps, int reg, uint8_t val)
{
	int ret;

	mutex_lock(&aps->mlock);
	ret = i2c_smbus_write_byte_data(aps->client, reg, val);
	if(ret < 0) {
/* < DTS2010072801000 zhangtao 20100728 begin */
		printk(KERN_ERR "%s: failed to write %d to reg[%d], err=%d\n", __FUNCTION__, val, reg, ret);
/* DTS2010072801000 zhangtao 20100728 end > */
	}
	mutex_unlock(&aps->mlock);

	return ret;
}

static int aps_12d_open(struct inode *inode, struct file *file)
{	
	PROXIMITY_DEBUG("aps_12d_open enter, timer_count=%d\n", aps_12d_timer_count);

	/* < DTS2010090300997 zhangtao 20100903 begin */
	/* when the device is open use this if light open report -1 when proximity open then lock it*/
	if( light_device_minor == iminor(inode) ){
		aps_first_read = 1;
		PROXIMITY_DEBUG("%s:light sensor open", __func__);
	}



	if( proximity_device_minor == iminor(inode) ){
		printk("%s:proximity_device_minor == iminor(inode)", __func__);
		wake_lock( &proximity_wake_lock);
		

		/* 0 is close, 1 is far */
		input_report_abs(this_aps_data->input_dev, ABS_DISTANCE, 1);			
		input_sync(this_aps_data->input_dev);
		PROXIMITY_DEBUG("%s:proximity = %d", __func__, 1);

	}
	/* DTS2010090300997 zhangtao 20100903 end > */

	if( 0 == aps_12d_timer_count )
		hrtimer_start(&this_aps_data->timer, ktime_set(1, 0), HRTIMER_MODE_REL);

	aps_12d_timer_count++;
	
	return nonseekable_open(inode, file);
}

static int aps_12d_release(struct inode *inode, struct file *file)
{
	PROXIMITY_DEBUG("aps_12d_release enter, timer_count=%d\n ", aps_12d_timer_count);

	aps_12d_timer_count--;
	
	if( 0 == aps_12d_timer_count ) {
		hrtimer_cancel(&this_aps_data->timer);
		aps_12d_delay = APS_12D_TIMRER;
	}
	/* < DTS2010090300997 zhangtao 20100903 begin */
	/*when proximity is released then unlock it*/
	if( proximity_device_minor == iminor(inode) ){
		printk("%s: proximity_device_minor == iminor(inode)", __func__);
		wake_unlock( &proximity_wake_lock);
	}
	/* DTS2010090300997 zhangtao 20100903 end > */
	   
	return 0;
}

static int
aps_12d_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
	   unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	short flag;

	switch (cmd) 
	{
		case ECS_IOCTL_APP_SET_LFLAG:
			if (copy_from_user(&flag, argp, sizeof(flag)))
				return -EFAULT;
				break;
		case ECS_IOCTL_APP_SET_PFLAG:
			if (copy_from_user(&flag, argp, sizeof(flag)))
				return -EFAULT;
				break;
		case ECS_IOCTL_APP_SET_DELAY:
          /*<BU5D08118 zhangtao 20100419 begin*/
			if (copy_from_user(&flag, argp, sizeof(flag)))
          /*BU5D08118 zhangtao 20100419 end>*/
				return -EFAULT;
				break;
		
			default:
				break;
	}

	switch (cmd) 
	{
		case ECS_IOCTL_APP_SET_LFLAG:
			atomic_set(&l_flag, flag);
			break;

		case ECS_IOCTL_APP_GET_LFLAG:  /*get open acceleration sensor flag*/
			flag = atomic_read(&l_flag);
			break;

		case ECS_IOCTL_APP_SET_PFLAG:
			atomic_set(&p_flag, flag);
/* <DTS2010100800714 liugaofei 20101008 begin */
			if( flag )
			{
				/*
				 * this means the proximity sensor is open.
				 * so init the range_index to zero 
				 */
				range_index = 0;
			}
/* DTS2010100800714 liugaofei 20101008 end */
			break;

		case ECS_IOCTL_APP_GET_PFLAG:  /*get open acceleration sensor flag*/
			flag = atomic_read(&p_flag);
			break;

		case ECS_IOCTL_APP_SET_DELAY:
			if(flag)
				aps_12d_delay = flag;
			else
/* < DTS2010102103994 zhangtao 20101112 begin */
				aps_12d_delay = 20;   /*200ms*/
/* DTS2010102103994 zhangtao 20101112 end > */
			break;

		case ECS_IOCTL_APP_GET_DELAY:
			flag = aps_12d_delay;
			break;
		/* < DTS2011071500961  liujinggang 20110715 begin */
		/*get value of proximity and light*/
		case ECS_IOCTL_APP_GET_PDATA_VALVE:
			flag = proximity_data_value;
       		break;
        
		case ECS_IOCTL_APP_GET_LDATA_VALVE:
       		flag = light_data_value;
       		break;
		/* DTS2011071500961  liujinggang 20110715 end > */
		default:
			break;
	}

	switch (cmd) 
	{
		case ECS_IOCTL_APP_GET_LFLAG:
			if (copy_to_user(argp, &flag, sizeof(flag)))
				return -EFAULT;
			break;

		case ECS_IOCTL_APP_GET_PFLAG:
			if (copy_to_user(argp, &flag, sizeof(flag)))
				return -EFAULT;
			break;

		case ECS_IOCTL_APP_GET_DELAY:
			if (copy_to_user(argp, &flag, sizeof(flag)))
			return -EFAULT;
			
			break;
		/* < DTS2011071500961  liujinggang 20110715 begin */
		/*get value of proximity and light*/
		case ECS_IOCTL_APP_GET_PDATA_VALVE:
       		if (copy_to_user(argp, &flag, sizeof(flag)))
           		return -EFAULT;
       		 break;
        
		case ECS_IOCTL_APP_GET_LDATA_VALVE:
      		if (copy_to_user(argp, &flag, sizeof(flag)))
          		return -EFAULT;
       		 break;
		/* DTS2011071500961  liujinggang 20110715 end > */
			
		default:
			break;
	}
	return 0;
	
}

static struct file_operations aps_12d_fops = {
	.owner = THIS_MODULE,
	.open = aps_12d_open,
	.release = aps_12d_release,
	.ioctl = aps_12d_ioctl,
};

static struct miscdevice light_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "light",
	.fops = &aps_12d_fops,
};

static struct miscdevice proximity_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "proximity",
	.fops = &aps_12d_fops,
};

static void aps_12d_work_func(struct work_struct *work)
{
/* < DTS2010072801000 zhangtao 20100728 begin */
	/* < DTS2010090300997 zhangtao 20100903 begin */
	int flag = -1;
	/* DTS2010090300997 zhangtao 20100903 end > */
	// delete flag_old
	int ret;
	int reg_val_lsb;
	int reg_val_msb;
	int	sesc = aps_12d_delay/1000;
	int nsesc = (aps_12d_delay%1000)*1000000;
	/* <DTS2010060100640 zhangtao 20100601 begin*/
	/* < DTS2010073101147 zhangtao 20100731 begin */
	/* < DTS2010081803338 zhangtao 20100818 begin */
/* < DTS2010102103994 zhangtao 20101112 begin */
	uint16_t high_threshold = 0;
    uint16_t low_threshold = 0;
/* DTS2010102103994 zhangtao 20101112 end > */
	/* DTS2010081803338 zhangtao 20100818 end > */
	/* DTS2010073101147 zhangtao 20100731 end > */
	/* DTS2010060100640 zhangtao 20100601 end>*/
	int ir_count = 0;
	int ps_count = 0;
	uint16_t als_count = 0;
	uint8_t als_level = 0;
	/* del als_level_old */
	uint8_t i;
/* DTS2010072801000 zhangtao 20100728 end > */
	struct aps_data *aps = container_of(work, struct aps_data, work);
	PROXIMITY_DEBUG("ghj aps_12d_work_func enter\n ");

	if (atomic_read(&p_flag))
	{
		/* Command 1 register: IR once */
/* <DTS2010100800714 liugaofei 20101008 begin */
		adjust_time = 0;
	re_adjust:
		/* init the range to the num last time we set */
		if(( range_index >=0 ) && ( range_index < TOTAL_RANGE_NUM ))
		{
			aps_i2c_reg_write(aps, APS_12D_REG_CMD2, \
/* < DTS2010102103994 zhangtao 20101112 begin */
					(uint8_t)(APS_12D_IRDR_SEL_50MA << 6 | \
						APS_12D_FREQ_SEL_DC << 4 | \
						APS_12D_RES_SEL_12 << 2 | \
						range_reg_value[range_index]));
			high_threshold = high_threshold_value[range_index];
			low_threshold = low_threshold_value[range_index];
/* DTS2010102103994 zhangtao 20101112 end > */
		}
		else
		{
			PROXIMITY_DEBUG("BUG: range_index error!!!!\n");
			range_index = 0;
		}
	er_adjust:
/* DTS2010100800714 liugaofei 20101008 end */
	    ret = aps_i2c_reg_write(aps, APS_12D_REG_CMD1, APS_12D_IR_ONCE);
/* < DTS2010102103994 zhangtao 20101112 begin */
	    msleep(45);
/* DTS2010102103994 zhangtao 20101112 end > */
	    reg_val_lsb = aps_i2c_reg_read(aps, APS_12D_DATA_LSB);
	    reg_val_msb = aps_i2c_reg_read(aps, APS_12D_DATA_MSB);
	    ir_count = ((uint16_t)reg_val_msb << 8) + (uint16_t)reg_val_lsb;
		
	    PROXIMITY_DEBUG("IR once lsb=%d; msb=%d; ir_count=%d \n", reg_val_lsb, reg_val_msb, ir_count);
/* < DTS2010072801000 zhangtao 20100728 begin */
	    if (ir_count > 0xFFF){
		    PROXIMITY_DEBUG("get wrong ir value, ir_count=%d \n", ir_count);
		    ir_count = 0xFFF;
	    }
	    if (ir_count < 0){
		    PROXIMITY_DEBUG("get wrong ir value, ir_count=%d \n", ir_count);
		    ir_count = 0;
	    }
/* <DTS2010100800714 liugaofei 20101008 begin */
		/*
		 * auto adjust the range
		 * stratety:
		 * if current adc value >= up_range_value[i]
		 *     switch to upper range
		 * if current adc value < down_range_value[i]
		 *     switch to lower range
		 */
		if(ir_count > up_range_value[range_index])
		{
			if(adjust_time < TOTAL_RANGE_NUM-1)
			{
				if(range_index < TOTAL_RANGE_NUM-1)
				{
					range_index++;
					adjust_time++;
					goto re_adjust;
				}
				else
				{
					PROXIMITY_DEBUG("infrared ray TOO HIGH?\n");
				}
			}
			else
			{
				PROXIMITY_DEBUG("proximity readjust exceed max retry times.\n");
			}
		}
		else if((ir_count < down_range_value[range_index]))
		{
			if(adjust_time < TOTAL_RANGE_NUM-1)
			{
				if(range_index >= TOTAL_RANGE_NUM-1)
				{
					range_index--;
					adjust_time++;
					goto re_adjust;
				}
				else
				{
					PROXIMITY_DEBUG("BUG: no exist lux value!!\n");
				}
			}
			else
			{
				PROXIMITY_DEBUG("proximity readjust exceed max retry times.\n");
			}
		}
/* DTS2010100800714 liugaofei 20101008 end */	
/* DTS2010072801000 zhangtao 20100728 end > */

	    ret = aps_i2c_reg_write(aps, APS_12D_REG_CMD1, APS_12D_PROXIMITY_ONCE);
/* < DTS2010102103994 zhangtao 20101112 begin */
	    msleep(45);
/* DTS2010102103994 zhangtao 20101112 end > */
	    reg_val_lsb = aps_i2c_reg_read(aps, APS_12D_DATA_LSB);
	    reg_val_msb = aps_i2c_reg_read(aps, APS_12D_DATA_MSB);
	    ps_count = ((uint16_t)reg_val_msb << 8) + (uint16_t)reg_val_lsb;
	    PROXIMITY_DEBUG("PS once lsb=%d; msb=%d; ps_count=%d \n", reg_val_lsb, reg_val_msb, ps_count);
/* < DTS2010072801000 zhangtao 20100728 begin */
	    if (ps_count > 0xFFF){
		    PROXIMITY_DEBUG("get wrong ps value, ps_count=%d \n", ps_count);
		    ps_count = 0xFFF;
	    }
	    if (ps_count < 0){
		    PROXIMITY_DEBUG("get wrong ps value, ps_count=%d \n", ps_count);
		    ps_count = 0;
	    }
/* <DTS2010100800714 liugaofei 20101008 begin */		
		/* < DTS2011071500961  liujinggang 20110715 begin */
		/*get value of proximity and light*/
	     proximity_data_value = ps_count;
		if (range_index == 1){     
		     light_data_value = ir_count*RANG_VALUE;
		}		     
		else {		     
		     light_data_value = ir_count;
		}		     
		/* DTS2011071500961  liujinggang 20110715 end > */
		if ((ps_count - ir_count) > err_threshold_value[range_index] || (ps_count - ir_count) < 0 )
			flag = -1;
	    else if( (ps_count - ir_count) > high_threshold )
/* DTS2010100800714 liugaofei 20101008 end */			
	     flag = 0;
	    else if( (ps_count - ir_count) < low_threshold )
	    	flag = 1;
	    else{
		    PROXIMITY_DEBUG("the value is in the threshold, do not report. \n");
	    }
/* < DTS2010102103994 zhangtao 20101112 begin */
        APS_DBG("the ps -ir is %d,the ps is %d,the ir is %d,the range_index is %d!\n",ps_count - ir_count,ps_count,ir_count,range_index);
/* DTS2010102103994 zhangtao 20101112 end > */
/* DTS2010072801000 zhangtao 20100728 end > */
/* <DTS2010100800714 liugaofei 20101008 begin */
		/* skip invalid event */
		if(-1 != flag)
		{
			if(1 == flag)
			{
				/* report far event immediately */
				/* 0 is close, 1 is far */
				input_report_abs(aps->input_dev, ABS_DISTANCE, flag);
				input_sync(aps->input_dev);
			}
			else if(last_event != flag)
			{
				PROXIMITY_DEBUG("NOTE: skip unstable data: %s !!!\n", flag ? "far" : "close");
				last_event = flag;
				goto er_adjust;
			}
			else
			{
				PROXIMITY_DEBUG("report distance flag=%d \n", flag);
				/* 0 is close, 1 is far */
				input_report_abs(aps->input_dev, ABS_DISTANCE, flag);
				input_sync(aps->input_dev);
			}
		}

		/* save the last event */
		last_event = flag;

		/*
		 * reset the sensor range to 1000.
		 */
		if(0 != range_index)
		{
			aps_i2c_reg_write(aps, APS_12D_REG_CMD2, \
/* < DTS2010102103994 zhangtao 20101112 begin */
					(uint8_t)(APS_12D_IRDR_SEL_50MA << 6 | \
						APS_12D_FREQ_SEL_DC << 4 | \
						APS_12D_RES_SEL_12 << 2 | \
						APS_12D_RANGE_SEL_ALS_1000));
/* DTS2010102103994 zhangtao 20101112 end > */
		}
	}
/* DTS2010100800714 liugaofei 20101008 end */

	if (atomic_read(&l_flag)) 
		{
		ret = aps_i2c_reg_write(aps, APS_12D_REG_CMD1, APS_12D_ALS_ONCE);
/* < DTS2010102103994 zhangtao 20101112 begin */
		msleep(45);
/* DTS2010102103994 zhangtao 20101112 end > */
		reg_val_lsb = aps_i2c_reg_read(aps, APS_12D_DATA_LSB);
		reg_val_msb = aps_i2c_reg_read(aps, APS_12D_DATA_MSB);
		als_count = ((uint16_t)reg_val_msb << 8) + (uint16_t)reg_val_lsb;
		PROXIMITY_DEBUG("ALS once lsb=%d; msb=%d; als_count=%d \n", reg_val_lsb, reg_val_msb, als_count);

/* < DTS2010072801000 zhangtao 20100728 begin */
		if (als_count > 0xFFF){
			PROXIMITY_DEBUG("get wrong als value, als_count=%d \n", als_count);
			als_count = 0xFFF;
		}

		als_level = LSENSOR_MAX_LEVEL - 1;
		for (i = 0; i < ARRAY_SIZE(lsensor_adc_table); i++){
			if (als_count < lsensor_adc_table[i]){
				als_level = i;
				break;
			}
		}
		PROXIMITY_DEBUG("report adc level=%d \n", als_level);
		
		if(aps_first_read)
		{
			/* report a invalid key first */
			aps_first_read = 0;
			/* < DTS2011010500959 zhangtao 20110119 begin */
			/* < DTS2011030404089 zhangtao 20110314 begin */
			/*changge the report event as -1 so the app will not changge anything*/
			input_report_abs(aps->input_dev, ABS_LIGHT, -1);
			/* DTS2011030404089 zhangtao 20110314 end > */
			/* DTS2011010500959 zhangtao 20110119 end > */
			input_sync(aps->input_dev);
		}
		else
		{
			/* < DTS2011042705601 zhangtao 20110427 begin */
			input_report_abs(aps->input_dev, ABS_LIGHT, als_level);
			/* DTS2011042705601 zhangtao 20110427 end > */
			input_sync(aps->input_dev);
/* DTS2010072801000 zhangtao 20100728 end > */
		}
	}
	
     if (atomic_read(&p_flag) || atomic_read(&l_flag))
		hrtimer_start(&aps->timer, ktime_set(sesc, nsesc), HRTIMER_MODE_REL);
	
}

static enum hrtimer_restart aps_timer_func(struct hrtimer *timer)
{
	struct aps_data *aps = container_of(timer, struct aps_data, timer);		
	queue_work(aps_wq, &aps->work);
	return HRTIMER_NORESTART;
}

/*<BU5D08118 zhangtao 20100419 begin*/
static int aps_12d_probe(
	
	struct i2c_client *client, const struct i2c_device_id *id)
{	
	int ret;
	struct aps_data *aps;
	/*the aps_12d sensors ispower on*/
	/* <BU5D07679 zhangtao 20100413 begin */
	struct vreg *vreg_gp4=NULL;
	int rc;
/* <DTS2010100800714 liugaofei 20101008 begin */
	int i;
/* DTS2010100800714 liugaofei 20101008 end */
	
    vreg_gp4 = vreg_get(NULL, VREG_GP4_NAME);
    /* < DTS2010061200552 zhangtao 20100612 begin */
    if (IS_ERR(vreg_gp4)) 
    {
	    pr_err("%s:gp4 power init get failed\n", __func__);
    }
    /* DTS2010061200552 zhangtao 20100612 end> */

    /* <DTS2011012600839 liliang 20110215 begin */
    /* set gp4 voltage as 2700mV for all */
    rc = vreg_set_level(vreg_gp4,VREG_GP4_VOLTAGE_VALUE_2700);
    /* <DTS2011012600839 liliang 20110215 end >*/
    
	if (rc) {
		PROXIMITY_DEBUG("%s: vreg_gp4  vreg_set_level failed \n", __func__);
		return rc;
	}
	rc = vreg_enable(vreg_gp4);
	if (rc) {
		pr_err("%s: vreg_gp4    vreg_enable failed \n", __func__);
		return rc;
	}
	mdelay(5);
       /* BU5D07679 zhangtao 20100413 end> */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		PROXIMITY_DEBUG(KERN_ERR "aps_12d_probe: need I2C_FUNC_I2C\n");
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}

	/* < DTS2010091001474 zhangtao 20100910 begin */
	/* if querry the board is T1 or T2 turn off the proximity */
    /*< DTS2010092400487  lijianzhao 20100924 begin */
    /* This modification for version A&B of U8800,only */
	if((machine_is_msm7x30_u8800())&&((get_hw_sub_board_id() == HW_VER_SUB_VA) || ((get_hw_sub_board_id() == HW_VER_SUB_VB))))
	{
		printk(KERN_ERR "aps_12d_probe: aps is not supported in U8800 and U8800 T1 board!\n");
		ret = -ENODEV;
		goto err_check_functionality_failed; 
	}    
    /* DTS2010092400487  lijianzhao 20100924 end >*/ 
	/* DTS2010091001474 zhangtao 20100910 end > */

	aps = kzalloc(sizeof(*aps), GFP_KERNEL);
	if (aps == NULL) {
		ret = -ENOMEM;
		goto err_alloc_data_failed;
	}

	mutex_init(&aps->mlock);
	
	INIT_WORK(&aps->work, aps_12d_work_func);
	aps->client = client;
	i2c_set_clientdata(client, aps);

	PROXIMITY_DEBUG(KERN_INFO "ghj aps_12d_probe send command 2\n ");
	
	/* Command 2 register: 25mA,DC,12bit,Range1 */

	/* < DTS2010081803338 zhangtao 20100818 begin */
	/* make the rang smaller can make the ir changge bigger */
	ret = aps_i2c_reg_write(aps, APS_12D_REG_CMD2, \
/* < DTS2010102103994 zhangtao 20101112 begin */
	                         (uint8_t)(APS_12D_IRDR_SEL_50MA << 6 | \
	                                   APS_12D_FREQ_SEL_DC << 4 | \
	                                   APS_12D_RES_SEL_12 << 2 | \
	                                   APS_12D_RANGE_SEL_ALS_1000));
/* DTS2010102103994 zhangtao 20101112 end > */
	/* DTS2010081803338 zhangtao 20100818 end > */
	if(ret < 0)
	{
		goto err_detect_failed;
	}
/* <DTS2010100800714 liugaofei 20101008 begin */
	range_index = 0;

	for(i = 0; i < TOTAL_RANGE_NUM; i++)
	{
		/* NOTE: do NOT use the last one */
		up_range_value[i] = MAX_ADC_OUTPUT - high_threshold_value[i] - RANGE_FIX; 
	}

	down_range_value[0] = 0;
	for(i = 1; i < TOTAL_RANGE_NUM; i++)
	{
		/* NOTE: do not use the first one */
		down_range_value[i] = (MAX_ADC_OUTPUT - high_threshold_value[i-1] - (MAX_ADC_OUTPUT / ADJUST_GATE)) / 4; 
	}
/* DTS2010100800714 liugaofei 20101008 end */

	/* < DTS2011042705601 zhangtao 20110427 begin */
	/*we don't use the input device sensors again */
	aps->input_dev = input_allocate_device();
	if (aps->input_dev == NULL) {
		ret = -ENOMEM;
		PROXIMITY_DEBUG(KERN_ERR "aps_12d_probe: Failed to allocate input device\n");
		goto err_input_dev_alloc_failed;
	}
	aps->input_dev->name = "sensors_aps";
	
	aps->input_dev->id.bustype = BUS_I2C;
	
	input_set_drvdata(aps->input_dev, aps);
	
	ret = input_register_device(aps->input_dev);
	if (ret) {
		printk(KERN_ERR "aps_probe: Unable to register %s input device\n", aps->input_dev->name);
		goto err_input_register_device_failed;
	}
	/* DTS2011042705601 zhangtao 20110427 end > */
	
	set_bit(EV_ABS, aps->input_dev->evbit);
	input_set_abs_params(aps->input_dev, ABS_LIGHT, 0, 10240, 0, 0);
	input_set_abs_params(aps->input_dev, ABS_DISTANCE, 0, 1, 0, 0);

	ret = misc_register(&light_device);
	if (ret) {
		printk(KERN_ERR "aps_12d_probe: light_device register failed\n");
		goto err_light_misc_device_register_failed;
	}

	ret = misc_register(&proximity_device);
	if (ret) {
		printk(KERN_ERR "aps_12d_probe: proximity_device register failed\n");
		goto err_proximity_misc_device_register_failed;
	}

	/* < DTS2010090300997 zhangtao 20100903 begin */

	if( light_device.minor != MISC_DYNAMIC_MINOR ){
		light_device_minor = light_device.minor;
	}

	

	if( proximity_device.minor != MISC_DYNAMIC_MINOR ){
		proximity_device_minor = proximity_device.minor ;
	}

	wake_lock_init(&proximity_wake_lock, WAKE_LOCK_SUSPEND, "proximity");
	/* DTS2010090300997 zhangtao 20100903 end > */


	hrtimer_init(&aps->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	aps->timer.function = aps_timer_func;
	
	aps_wq = create_singlethread_workqueue("aps_wq");

	if (!aps_wq) 
	{
		ret = -ENOMEM;
		goto err_create_workqueue_failed;
	}
	
	this_aps_data =aps;

    /* <DTS2011032104626 shenjinming 20110321 begin */
    #ifdef CONFIG_HUAWEI_HW_DEV_DCT
    /* detect current device successful, set the flag as present */
    set_hw_dev_flag(DEV_I2C_APS);
    #endif
    /* <DTS2011032104626 shenjinming 20110321 end> */
/* < DTS2011052606009 jiaxianghong 20110527 end */	  

	printk(KERN_INFO "aps_12d_probe: Start Proximity Sensor APS-12D\n");
/* DTS2010072801000 zhangtao 20100728 end > */

	return 0;
	
err_create_workqueue_failed:
	misc_deregister(&proximity_device);
err_proximity_misc_device_register_failed:
	misc_deregister(&light_device);
err_light_misc_device_register_failed:
err_input_register_device_failed:
	input_free_device(aps->input_dev);
err_input_dev_alloc_failed:
err_detect_failed:
	kfree(aps);
err_alloc_data_failed:
err_check_functionality_failed:
/* < DTS2010061200552 zhangtao 20100612 begin */
	if(NULL != vreg_gp4)
	{
	    /* < DTS2011052101089 shenjinming 20110521 begin */
        /* can't use the flag ret here, it will change the return value of probe function */
        vreg_disable(vreg_gp4);
        /* delete a line */
        /* DTS2011052101089 shenjinming 20110521 end > */
	}
/* DTS2010061200552 zhangtao 20100612 end > */
	return ret;
  
}
/*BU5D08118 zhangtao 20100419 end>*/
static int aps_12d_remove(struct i2c_client *client)
{
	struct aps_data *aps = i2c_get_clientdata(client);

	PROXIMITY_DEBUG("ghj aps_12d_remove enter\n ");

	hrtimer_cancel(&aps->timer);

	misc_deregister(&light_device);
	misc_deregister(&proximity_device);

	input_unregister_device(aps->input_dev);

	kfree(aps);
	return 0;
}

static int aps_12d_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int ret;
	struct aps_data *aps = i2c_get_clientdata(client);

	PROXIMITY_DEBUG("ghj aps_12d_suspend enter\n ");

	hrtimer_cancel(&aps->timer);
	ret = cancel_work_sync(&aps->work);

	ret = aps_i2c_reg_write(aps, APS_12D_REG_CMD1, APS_12D_POWER_DOWN);

	if (aps->power) {
		ret = aps->power(0);
		if (ret < 0)
/* < DTS2010072801000 zhangtao 20100728 begin */
			printk(KERN_ERR "aps_12d_suspend power off failed\n");
	}

	/* enable aps_first_read */
/* < DTS2010090300997 zhangtao 20100903 begin */
/* the flag is not used */
/* DTS2010090300997 zhangtao 20100903 end > */
/* DTS2010072801000 zhangtao 20100728 end > */
	return 0;
}

static int aps_12d_resume(struct i2c_client *client)
{
	int ret;
	struct aps_data *aps = i2c_get_clientdata(client);

	PROXIMITY_DEBUG("ghj aps_12d_resume enter\n ");

	/* Command 2 register: 25mA,DC,12bit,Range2 */
	/* < DTS2010081803338 zhangtao 20100818 begin */
	/* make the rang smaller can make the ir changge bigger */
	ret = aps_i2c_reg_write(aps, APS_12D_REG_CMD2, \
/* < DTS2010102103994 zhangtao 20101112 begin */
	                         (uint8_t)(APS_12D_IRDR_SEL_50MA << 6 | \
	                                   APS_12D_FREQ_SEL_DC << 4 | \
	                                   APS_12D_RES_SEL_12 << 2 | \
	                                   APS_12D_RANGE_SEL_ALS_1000));
/* DTS2010102103994 zhangtao 20101112 end > */
	/* DTS2010081803338 zhangtao 20100818 end > */

/* < DTS2010072801000 zhangtao 20100728 begin */
	/* enable aps_first_read */
/* < DTS2010090300997 zhangtao 20100903 begin */
/* the flag is not used */
/* DTS2010090300997 zhangtao 20100903 end > */
/* DTS2010072801000 zhangtao 20100728 end > */
	hrtimer_start(&aps->timer, ktime_set(1, 0), HRTIMER_MODE_REL);

	return 0;
}

static const struct i2c_device_id aps_id[] = {
	{ "aps-12d", 0 },
	{ }
};

static struct i2c_driver aps_driver = {
	.probe		= aps_12d_probe,
	.remove		= aps_12d_remove,
	.suspend	= aps_12d_suspend,
	.resume		= aps_12d_resume,
	.id_table	= aps_id,
	.driver = {
		.name	="aps-12d",
	},
};

static int __devinit aps_12d_init(void)
{
/* < DTS2010072801000 zhangtao 20100728 begin */
/* delete some lines just log*/
/* DTS2010072801000 zhangtao 20100728 end > */
	return i2c_add_driver(&aps_driver);
}

static void __exit aps_12d_exit(void)
{
	i2c_del_driver(&aps_driver);
	if (aps_wq)
		destroy_workqueue(aps_wq);
}

device_initcall_sync(aps_12d_init);
module_exit(aps_12d_exit);
/* BU5D05131 gaohuajiang 20100318 end > */

MODULE_DESCRIPTION("Proximity Driver");
MODULE_LICENSE("GPL");
