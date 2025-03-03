#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>
#include <linux/extcon.h>
#include <linux/power/battery_id.h>
#include <linux/notifier.h>
#include <linux/usb/otg.h>
#include "power_supply.h"
#include "power_supply_charger.h"

struct work_struct notifier_work;
#define MAX_CHARGER_COUNT 5

static LIST_HEAD(algo_list);

struct power_supply_charger {
	bool is_cable_evt_reg;
	/*cache battery and charger properties */
	struct list_head chrgr_cache_lst;
	struct list_head batt_cache_lst;
	struct list_head evt_queue;
	struct work_struct algo_trigger_work;
	struct work_struct wireless_chrgr_work;
	struct mutex evt_lock;
	struct power_supply_cable_props cable_props;
};

static struct power_supply_charger psy_chrgr;

static struct power_supply_cable_props cable_list[] = {
	{
		.chrg_type = POWER_SUPPLY_CHARGER_TYPE_USB_SDP,
		.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT,
	},
	{
		.chrg_type = POWER_SUPPLY_CHARGER_TYPE_USB_CDP,
		.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT,
	},
	{
		.chrg_type = POWER_SUPPLY_CHARGER_TYPE_USB_DCP,
		.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT,
	},
	{
		.chrg_type = POWER_SUPPLY_CHARGER_TYPE_USB_ACA,
		.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT,
	},
	{
		.chrg_type = POWER_SUPPLY_CHARGER_TYPE_ACA_DOCK,
		.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT,
	},
	{
		.chrg_type = POWER_SUPPLY_CHARGER_TYPE_SE1,
		.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT,
	},
	{
		.chrg_type = POWER_SUPPLY_CHARGER_TYPE_USB_TYPEC,
		.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT,
	},
	{
		.chrg_type = POWER_SUPPLY_CHARGER_TYPE_AC,
		.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT,
	},
	{
		.chrg_type = POWER_SUPPLY_CHARGER_TYPE_WIRELESS,
		.chrg_evt = POWER_SUPPLY_CHARGER_EVENT_DISCONNECT,
	},
};

static int get_supplied_by_list(struct power_supply *psy,
				struct power_supply *psy_lst[]);

static int handle_cable_notification(struct notifier_block *nb,
				   unsigned long event, void *data);
struct usb_phy *otg_xceiver;
struct notifier_block usb_nb = {
		   .notifier_call = handle_cable_notification,
		};
struct notifier_block psy_nb = {
		   .notifier_call = handle_cable_notification,
		};

static void configure_chrgr_source(struct power_supply_cable_props *cable_lst);

struct power_supply_cable_props *get_cable(unsigned long usb_chrgr_type)
{

	switch (usb_chrgr_type) {
	case POWER_SUPPLY_CHARGER_TYPE_USB_SDP:
		return &cable_list[0];
	case POWER_SUPPLY_CHARGER_TYPE_USB_CDP:
		return &cable_list[1];
	case POWER_SUPPLY_CHARGER_TYPE_USB_DCP:
		return &cable_list[2];
	case POWER_SUPPLY_CHARGER_TYPE_USB_ACA:
		return &cable_list[3];
	case POWER_SUPPLY_CHARGER_TYPE_ACA_DOCK:
		return &cable_list[4];
	case POWER_SUPPLY_CHARGER_TYPE_SE1:
		return &cable_list[5];
	case POWER_SUPPLY_CHARGER_TYPE_USB_TYPEC:
		return &cable_list[6];
	case POWER_SUPPLY_CHARGER_TYPE_AC:
		return &cable_list[7];
	case POWER_SUPPLY_CHARGER_TYPE_WIRELESS:
		return &cable_list[8];
	}

	return NULL;
}


static void notifier_event_worker(struct work_struct *work)
{
	configure_chrgr_source(cable_list);
}

static int process_cable_props(struct power_supply_cable_props *cap)
{

	struct power_supply_cable_props *cable = NULL;
	pr_info("%s: event:%d, type:%d, ma:%d\n",
		__func__, cap->chrg_evt, cap->chrg_type, cap->ma);

	cable = get_cable(cap->chrg_type);

	if (!cable) {
		pr_err("%s:Error in getting charger cable\n", __func__);
		return -EINVAL;
	}
	if ((cable->chrg_evt != cap->chrg_evt) ||
		(cable->ma != cap->ma)) {
		cable->chrg_evt = cap->chrg_evt;
		cable->ma = cap->ma;
		schedule_work(&notifier_work);
	}

	return 0;
}

static int handle_cable_notification(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct power_supply_cable_props cap;
	struct power_supply_cable_props *cable = NULL;

	if (event != USB_EVENT_CHARGER && event != PSY_CABLE_EVENT &&
				event != USB_EVENT_ENUMERATED)
		return NOTIFY_DONE;
	if (event == USB_EVENT_ENUMERATED) {
		cable = get_cable(POWER_SUPPLY_CHARGER_TYPE_USB_SDP);
		if (cable->chrg_evt ==
			POWER_SUPPLY_CHARGER_EVENT_DISCONNECT)
			return NOTIFY_DONE;
	}

	if (!data)
		return NOTIFY_DONE;

	cap = *(struct power_supply_cable_props *)data;

	process_cable_props(&cap);

	return NOTIFY_OK;
}

static int register_psy_notifier(void)
{
	int retval;

	pr_info("%s\n", __func__);
	retval = power_supply_reg_notifier(&psy_nb);
	if (retval) {
		pr_err("failure to register power_supply notifier\n");
		return retval;
	}

	INIT_WORK(&notifier_work, notifier_event_worker);

	return retval;
}

static int register_usb_notifier(void)
{
	int retval;

	pr_info("%s\n", __func__);
	otg_xceiver = usb_get_phy(USB_PHY_TYPE_USB2);
	if (!otg_xceiver) {
		pr_err("failure to get otg transceiver\n");
		retval = -EIO;
		goto notifier_reg_failed;
	}
	retval = usb_register_notifier(otg_xceiver, &usb_nb);
	if (retval) {
		pr_err("failure to register otg notifier\n");
		goto notifier_reg_failed;
	}
notifier_reg_failed:
	return retval;
}


static int charger_cable_notifier(struct notifier_block *nb,
				  unsigned long event, void *ptr);
static void charger_cable_event_worker(struct work_struct *work);
struct charging_algo *power_supply_get_charging_algo
		(struct power_supply *, struct ps_batt_chg_prof *);

static void init_charger_cables(struct power_supply_cable_props *cable_lst,
					int count)
{
	struct power_supply_cable_props cap;

	register_psy_notifier();

	if (!otg_get_chrg_status(otg_xceiver, &cap))
		process_cable_props(&cap);
}

static inline int is_charging_can_be_enabled(struct power_supply *psy)
{
	int health;

	health = HEALTH(psy);
	if (IS_BATTERY(psy)) {
		return (health == POWER_SUPPLY_HEALTH_GOOD) ||
				(health == POWER_SUPPLY_HEALTH_DEAD);
	} else {
		int throttle_action, inlmt;

		throttle_action = CURRENT_THROTTLE_ACTION(psy);
		inlmt = INLMT(psy);

		return (throttle_action != PSY_THROTTLE_DISABLE_CHARGER) &&
			(throttle_action != PSY_THROTTLE_DISABLE_CHARGING) &&
			(inlmt >= 100) && (health == POWER_SUPPLY_HEALTH_GOOD);
	}
}

static inline void get_cur_chrgr_prop(struct power_supply *psy,
				      struct charger_props *chrgr_prop)
{
	chrgr_prop->is_charging = IS_CHARGING_ENABLED(psy);
	chrgr_prop->name = psy->name;
	chrgr_prop->online = IS_ONLINE(psy);
	chrgr_prop->present = IS_PRESENT(psy);
	chrgr_prop->cable = CABLE_TYPE(psy);
	chrgr_prop->health = HEALTH(psy);
	chrgr_prop->tstamp = get_jiffies_64();
	chrgr_prop->throttle_state = CURRENT_THROTTLE_STATE(psy);

}

static inline int get_chrgr_prop_cache(struct power_supply *psy,
				       struct charger_props *chrgr_cache)
{

	struct charger_props *chrgr_prop;
	int ret = -ENODEV;

	list_for_each_entry(chrgr_prop, &psy_chrgr.chrgr_cache_lst, node) {
		if (!strcmp(chrgr_prop->name, psy->name)) {
			memcpy(chrgr_cache, chrgr_prop, sizeof(*chrgr_cache));
			ret = 0;
			break;
		}
	}

	return ret;
}

static void dump_charger_props(struct charger_props *props)
{
	pr_devel("%s:name=%s present=%d is_charging=%d health=%d online=%d cable=%ld tstamp=%llu\n",
		__func__, props->name, props->present, props->is_charging,
		props->health, props->online, props->cable, props->tstamp);
}

static void dump_battery_props(struct batt_props *props)
{
	pr_devel("%s:name=%s voltage_now=%ld current_now=%ld temperature=%d status=%ld health=%d tstamp=%llu algo_stat=%d ",
		__func__, props->name, props->voltage_now, props->current_now,
		props->temperature, props->status, props->health,
		props->tstamp, props->algo_stat);
}

static inline void cache_chrgr_prop(struct charger_props *chrgr_prop_new)
{

	struct charger_props *chrgr_cache;

	list_for_each_entry(chrgr_cache, &psy_chrgr.chrgr_cache_lst, node) {
		if (!strcmp(chrgr_cache->name, chrgr_prop_new->name))
			goto update_props;
	}

	chrgr_cache = kzalloc(sizeof(*chrgr_cache), GFP_KERNEL);
	if (chrgr_cache == NULL) {
		pr_err("%s:Error in allocating memory\n", __func__);
		return;
	}

	INIT_LIST_HEAD(&chrgr_cache->node);
	list_add_tail(&chrgr_cache->node, &psy_chrgr.chrgr_cache_lst);

	chrgr_cache->name = chrgr_prop_new->name;

update_props:
	chrgr_cache->is_charging = chrgr_prop_new->is_charging;
	chrgr_cache->online = chrgr_prop_new->online;
	chrgr_cache->health = chrgr_prop_new->health;
	chrgr_cache->present = chrgr_prop_new->present;
	chrgr_cache->cable = chrgr_prop_new->cable;
	chrgr_cache->tstamp = chrgr_prop_new->tstamp;
	chrgr_cache->throttle_state = chrgr_prop_new->throttle_state;
}

static inline bool is_chrgr_prop_changed(struct power_supply *psy)
{
	struct charger_props chrgr_prop_cache, chrgr_prop;

	get_cur_chrgr_prop(psy, &chrgr_prop);
	/* Get cached battery property. If no cached property available
	 *  then cache the new property and return true
	 */
	if (get_chrgr_prop_cache(psy, &chrgr_prop_cache)) {
		cache_chrgr_prop(&chrgr_prop);
		return true;
	}

	dump_charger_props(&chrgr_prop);
	dump_charger_props(&chrgr_prop_cache);

	if (!IS_CHARGER_PROP_CHANGED(chrgr_prop, chrgr_prop_cache))
		return false;

	cache_chrgr_prop(&chrgr_prop);
	return true;
}
static void cache_successive_samples(long *sample_array, long new_sample)
{
	int i;

	for (i = 0; i < MAX_CUR_VOLT_SAMPLES - 1; ++i)
		*(sample_array + i) = *(sample_array + i + 1);

	*(sample_array + i) = new_sample;
}

static inline void cache_bat_prop(struct batt_props *bat_prop_new, bool force)
{

	struct batt_props *bat_cache;

	/* Find entry in cache list. If an entry is located update
	 * the existing entry else create new entry in the list */
	list_for_each_entry(bat_cache, &psy_chrgr.batt_cache_lst, node) {
		if (!strcmp(bat_cache->name, bat_prop_new->name))
			goto update_props;
	}

	bat_cache = kzalloc(sizeof(*bat_cache), GFP_KERNEL);
	if (bat_cache == NULL) {
		pr_err("%s:Error in allocating memory\n", __func__);
		return;
	}
	INIT_LIST_HEAD(&bat_cache->node);
	list_add_tail(&bat_cache->node, &psy_chrgr.batt_cache_lst);

	bat_cache->name = bat_prop_new->name;

update_props:
	if (time_after64(bat_prop_new->tstamp,
		(bat_cache->tstamp + DEF_CUR_VOLT_SAMPLE_JIFF)) || force ||
						bat_cache->tstamp == 0) {
		cache_successive_samples(bat_cache->voltage_now_cache,
						bat_prop_new->voltage_now);
		cache_successive_samples(bat_cache->current_now_cache,
						bat_prop_new->current_now);
		bat_cache->tstamp = bat_prop_new->tstamp;
	}

	bat_cache->voltage_now = bat_prop_new->voltage_now;
	bat_cache->current_now = bat_prop_new->current_now;
	bat_cache->health = bat_prop_new->health;

	bat_cache->temperature = bat_prop_new->temperature;
	bat_cache->status = bat_prop_new->status;
	bat_cache->algo_stat = bat_prop_new->algo_stat;
}

static inline int get_bat_prop_cache(struct power_supply *psy,
				     struct batt_props *bat_cache)
{
	struct batt_props *bat_prop;
	int ret = -ENODEV;

	list_for_each_entry(bat_prop, &psy_chrgr.batt_cache_lst, node) {
		if (!strcmp(bat_prop->name, psy->name)) {
			memcpy(bat_cache, bat_prop, sizeof(*bat_cache));
			ret = 0;
			break;
		}
	}

	return ret;
}

static inline void get_cur_bat_prop(struct power_supply *psy,
				    struct batt_props *bat_prop)
{
	struct batt_props bat_prop_cache;
	int ret;

	bat_prop->name = psy->name;
	bat_prop->voltage_now = VOLTAGE_OCV(psy) / 1000;
	bat_prop->current_now = CURRENT_NOW(psy) / 1000;
	bat_prop->temperature = TEMPERATURE(psy) / 10;
	bat_prop->status = STATUS(psy);
	bat_prop->health = HEALTH(psy);
	bat_prop->tstamp = get_jiffies_64();

	/* Populate cached algo data to new profile */
	ret = get_bat_prop_cache(psy, &bat_prop_cache);
	if (!ret)
		bat_prop->algo_stat = bat_prop_cache.algo_stat;
}

static inline bool is_batt_prop_changed(struct power_supply *psy)
{
	struct batt_props bat_prop_cache, bat_prop;

	/* Get cached battery property. If no cached property available
	 *  then cache the new property and return true
	 */
	get_cur_bat_prop(psy, &bat_prop);
	if (get_bat_prop_cache(psy, &bat_prop_cache)) {
		cache_bat_prop(&bat_prop, false);
		return true;
	}

	dump_battery_props(&bat_prop);
	dump_battery_props(&bat_prop_cache);

	if (!IS_BAT_PROP_CHANGED(bat_prop, bat_prop_cache))
		return false;

	cache_bat_prop(&bat_prop, false);
	return true;
}

static inline bool is_supplied_to_has_ext_pwr_changed(struct power_supply *psy)
{
	int i;
	struct power_supply *psb;
	bool is_pwr_changed_defined = true;

	for (i = 0; i < psy->num_supplicants; i++) {
		psb =
		    power_supply_get_by_name(psy->
					     supplied_to[i]);
		if (psb && !psb->external_power_changed)
			is_pwr_changed_defined &= false;
	}

	return is_pwr_changed_defined;

}

static inline bool is_supplied_by_changed(struct power_supply *psy)
{
	int cnt;
	struct power_supply *chrgr_lst[MAX_CHARGER_COUNT];

	cnt = get_supplied_by_list(psy, chrgr_lst);
	while (cnt--) {
		if ((IS_CHARGER(chrgr_lst[cnt])) &&
			is_chrgr_prop_changed(chrgr_lst[cnt]))
			return true;
	}

	return false;
}

static inline bool is_trigger_charging_algo(struct power_supply *psy)
{
	/* trigger charging alorithm if battery or
	 * charger properties are changed. Also no need to
	 * invoke algorithm for power_supply_changed from
	 * charger, if all supplied_to has the ext_port_changed defined.
	 * On invoking the ext_port_changed the supplied to can send
	 * power_supplied_changed event.
	 */
	if ((IS_CHARGER(psy) && !is_supplied_to_has_ext_pwr_changed(psy)) &&
			is_chrgr_prop_changed(psy))
		return true;

	if ((IS_BATTERY(psy)) && (is_batt_prop_changed(psy) ||
				is_supplied_by_changed(psy)))
		return true;

	return false;
}

static int get_supplied_by_list(struct power_supply *psy,
				struct power_supply *psy_lst[])
{
	struct class_dev_iter iter;
	struct device *dev;
	struct power_supply *pst;
	int cnt = 0, i, j;

	if (!IS_BATTERY(psy))
		return 0;

	/* Identify chargers which are supplying power to the battery */
	class_dev_iter_init(&iter, power_supply_class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter))) {
		pst = (struct power_supply *)dev_get_drvdata(dev);
		if (!IS_CHARGER(pst))
			continue;
		for (i = 0; i < pst->num_supplicants; i++) {
			if (!strcmp(pst->supplied_to[i], psy->name))
				psy_lst[cnt++] = pst;
		}
	}
	class_dev_iter_exit(&iter);

	if (cnt <= 1)
		return cnt;

	/*sort based on priority. 0 has the highest priority  */
	for (i = 0; i < cnt; ++i)
		for (j = 0; j < cnt; ++j)
			if (PRIORITY(psy_lst[j]) > PRIORITY(psy_lst[i]))
				swap(psy_lst[j], psy_lst[i]);

	return cnt;
}

static void update_charger_online(struct power_supply *psy)
{
	if (IS_PRESENT(psy))
		set_charger_online(psy, 1);
	else
		set_charger_online(psy, 0);
}

static inline void cache_cur_batt_prop_force(struct power_supply *psb)
{
	struct batt_props bat_prop;

	if (!IS_BATTERY(psb))
		return;

	get_cur_bat_prop(psb, &bat_prop);
	cache_bat_prop(&bat_prop, true);
}

static void update_sysfs(struct power_supply *psy)
{
	int i, cnt;
	struct power_supply *psb;
	struct power_supply *chrgr_lst[MAX_CHARGER_COUNT];

	if (IS_BATTERY(psy)) {
		/* set charger online */
		cnt = get_supplied_by_list(psy, chrgr_lst);
		while (cnt--) {
			if (!IS_PRESENT(chrgr_lst[cnt]))
				continue;

			update_charger_online(chrgr_lst[cnt]);
		}
	} else {
		/*set charger online */
		update_charger_online(psy);
	}
}

static int trigger_algo(struct power_supply *psy)
{
	unsigned long cc = 0, cv = 0, cc_min;
	struct power_supply *chrgr_lst[MAX_CHARGER_COUNT];
	struct batt_props bat_prop;
	struct charging_algo *algo;
	struct ps_batt_chg_prof chrg_profile;
	int cnt, ret;

	if (psy->type != POWER_SUPPLY_TYPE_BATTERY)
		return 0;

	if (get_batt_prop(&chrg_profile)) {
		pr_err("%s:Error in getting charge profile\n", __func__);
		return -EINVAL;
	}


	ret = get_bat_prop_cache(psy, &bat_prop);
	if (ret) {
		pr_warn("%s:Error in getting the battery property of %s!!\n",
							__func__, psy->name);

		/*
		 * battery properties are not cached.
		 * get the current properties to process
		 */
		get_cur_bat_prop(psy, &bat_prop);
	}

	algo = power_supply_get_charging_algo(psy, &chrg_profile);
	if (!algo) {
		pr_err("%s:Error in getting charging algo!!\n", __func__);
		return -EINVAL;
	}

	algo->get_next_cc_cv(bat_prop, chrg_profile, &cc, &cv);


	cache_bat_prop(&bat_prop, false);

	if (!cc || !cv)
		return -ENODATA;

	/* CC needs to be updated for all chargers which are supplying
	 *  power to this battery to ensure that the sum of CCs of all
	 * chargers are never more than the CC selected by the algo.
	 * The CC is set based on the charger priority.
	 */
	cnt = get_supplied_by_list(psy, chrgr_lst);

	while (cnt--) {
		if (!IS_PRESENT(chrgr_lst[cnt]))
			continue;

		cc_min = min_t(unsigned long, MAX_CC(chrgr_lst[cnt]), cc);
		if (cc_min < 0)
			cc_min = 0;
		cc -= cc_min;
		set_cc(chrgr_lst[cnt], cc_min);
		set_cv(chrgr_lst[cnt], cv);
	}

	return 0;
}

static void __power_supply_trigger_charging_handler(struct power_supply *psy)
{
	mutex_lock(&psy_chrgr.evt_lock);

	if (is_trigger_charging_algo(psy)) {
		trigger_algo(psy);
		update_sysfs(psy);
		power_supply_changed(psy);
	}
	mutex_unlock(&psy_chrgr.evt_lock);

}

static int __trigger_charging_handler(struct device *dev, void *data)
{
	struct power_supply *psy = dev_get_drvdata(dev);

	__power_supply_trigger_charging_handler(psy);

	return 0;
}

static void handle_wireless_charger(struct work_struct *work)
{
	atomic_notifier_call_chain(&power_supply_notifier,
		PSY_CABLE_EVENT, &psy_chrgr.cable_props);
}

static void trigger_algo_psy_class(struct work_struct *work)
{
	class_for_each_device(power_supply_class, NULL, NULL,
			__trigger_charging_handler);
}

void power_supply_trigger_charging_handler(struct power_supply *psy)
{
	if (!psy_chrgr.is_cable_evt_reg)
		return;


	if (psy)
		__power_supply_trigger_charging_handler(psy);
	else
		schedule_work(&psy_chrgr.algo_trigger_work);
}
EXPORT_SYMBOL(power_supply_trigger_charging_handler);

static inline int get_battery_thresholds(struct power_supply *psy,
	struct psy_batt_thresholds *bat_thresh)
{
	struct charging_algo *algo;
	struct ps_batt_chg_prof chrg_profile;

	/* FIXME: Get iterm only for supplied_to arguments*/
	if (get_batt_prop(&chrg_profile)) {
		pr_err("%s:Error in getting charge profile\n", __func__);
		return -EINVAL;
	}

	algo = power_supply_get_charging_algo(psy, &chrg_profile);
	if (!algo) {
		pr_err("%s:Error in getting charging algo!!\n", __func__);
		return -EINVAL;
	}

	if (algo->get_batt_thresholds) {
		algo->get_batt_thresholds(chrg_profile, bat_thresh);
	} else {
		pr_err("%s:Error in getting battery thresholds from: %s\n",
			__func__, algo->name);
		return -EINVAL;
	}
	return 0;
}

static int select_chrgr_cable(struct device *dev, void *data)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct power_supply_cable_props *cable, *max_ma_cable = NULL;
	struct power_supply_cable_props *cable_lst =
		(struct power_supply_cable_props *)data;
	int max_ma = -1, i;

	if (!IS_CHARGER(psy))
		return 0;
	mutex_lock(&psy_chrgr.evt_lock);
	/* get cable with maximum capability */
	for (i = 0; i < ARRAY_SIZE(cable_list); ++i) {
		cable = cable_lst + i;
		if ((!IS_CABLE_ACTIVE(cable->chrg_evt)) ||
		    (!IS_SUPPORTED_CABLE(psy, cable->chrg_type)))
			continue;

		if ((int)cable->ma > max_ma) {
			max_ma_cable = cable;
			max_ma = cable->ma;
		}
	}

	/* no cable connected. disable charging */
	if (!max_ma_cable) {
		/* set present and online as 0 */
		enable_hvdcp(psy, 0);
		set_present(psy, 0);
		update_charger_online(psy);
		switch_cable(psy, POWER_SUPPLY_CHARGER_TYPE_NONE);
		mutex_unlock(&psy_chrgr.evt_lock);
		power_supply_changed(psy);

		return 0;
	}

	/* cable type changed.New cable connected or existing cable
	 * capabilities changed.switch cable and enable charger and charging
	 */
	set_present(psy, 1);
	update_charger_online(psy);
	power_supply_changed(psy);
	if (CABLE_TYPE(psy) != max_ma_cable->chrg_type) {
		if (max_ma_cable->chrg_evt ==
				POWER_SUPPLY_CHARGER_EVENT_ATTACH) {
			/* send SELECT notification for cable selected */
			max_ma_cable->chrg_evt =
				POWER_SUPPLY_CHARGER_EVENT_SELECTED;
			psy_chrgr.cable_props.chrg_type =
				max_ma_cable->chrg_type;
			psy_chrgr.cable_props.chrg_evt =
				max_ma_cable->chrg_evt;
			psy_chrgr.cable_props.ma = max_ma_cable->ma;
			schedule_work(&psy_chrgr.wireless_chrgr_work);
		}
		switch_cable(psy, max_ma_cable->chrg_type);
	}

	mutex_unlock(&psy_chrgr.evt_lock);
	power_supply_trigger_charging_handler(NULL);
	/* Cable status is same as previous. No action to be taken */

	return 0;
}

static void configure_chrgr_source(struct power_supply_cable_props *cable_lst)
{
	class_for_each_device(power_supply_class, NULL,
			      cable_lst, select_chrgr_cable);
}

int psy_charger_throttle_charger(struct power_supply *psy,
					unsigned long state)
{
	int ret = 0;

	if (!psy)
		return -EINVAL;

	if (state < 0 || state >= MAX_THROTTLE_STATE(psy))
		return -EINVAL;

	mutex_lock(&psy_chrgr.evt_lock);

	switch THROTTLE_ACTION(psy, state)
	{
		case PSY_THROTTLE_CC_LIMIT:
			SET_MAX_CC(psy, THROTTLE_VALUE(psy, state));
			break;
		case PSY_THROTTLE_INPUT_LIMIT:
			set_inlmt(psy, THROTTLE_VALUE(psy, state));
			break;
		default:
			pr_err("%s:Invalid throttle action for %s\n",
						__func__, psy->name);
			ret = -EINVAL;
			break;
	}
	mutex_unlock(&psy_chrgr.evt_lock);

	/* Configure the driver based on new state */
	if (!ret)
		configure_chrgr_source(cable_list);

	return ret;
}
EXPORT_SYMBOL(psy_charger_throttle_charger);

int power_supply_register_charger(struct power_supply *psy)
{
	int ret = 0;

	if (!psy_chrgr.is_cable_evt_reg) {
		mutex_init(&psy_chrgr.evt_lock);
		init_charger_cables(cable_list, ARRAY_SIZE(cable_list));
		INIT_LIST_HEAD(&psy_chrgr.chrgr_cache_lst);
		INIT_LIST_HEAD(&psy_chrgr.batt_cache_lst);
		INIT_WORK(&psy_chrgr.algo_trigger_work, trigger_algo_psy_class);
		INIT_WORK(&psy_chrgr.wireless_chrgr_work,
			handle_wireless_charger);
		psy_chrgr.is_cable_evt_reg = true;
	}
	psy_chrgr.is_cable_evt_reg = true;
	return ret;
}
EXPORT_SYMBOL(power_supply_register_charger);

static int __init power_supply_charger_init(void)
{
	mutex_init(&psy_chrgr.evt_lock);
	init_charger_cables(cable_list, ARRAY_SIZE(cable_list));
	INIT_LIST_HEAD(&psy_chrgr.chrgr_cache_lst);
	INIT_LIST_HEAD(&psy_chrgr.batt_cache_lst);
	INIT_WORK(&psy_chrgr.algo_trigger_work, trigger_algo_psy_class);
	INIT_WORK(&psy_chrgr.wireless_chrgr_work,
		handle_wireless_charger);

	return 0;
}

enum power_supply_type get_power_supply_type(
		enum power_supply_charger_cable_type cable)
{

	switch (cable) {

	case POWER_SUPPLY_CHARGER_TYPE_USB_DCP:
		return POWER_SUPPLY_TYPE_USB_DCP;
	case POWER_SUPPLY_CHARGER_TYPE_USB_CDP:
		return POWER_SUPPLY_TYPE_USB_CDP;
	case POWER_SUPPLY_CHARGER_TYPE_USB_ACA:
	case POWER_SUPPLY_CHARGER_TYPE_ACA_DOCK:
		return POWER_SUPPLY_TYPE_USB_ACA;
	case POWER_SUPPLY_CHARGER_TYPE_AC:
		return POWER_SUPPLY_TYPE_MAINS;
	case POWER_SUPPLY_CHARGER_TYPE_WIRELESS:
		return POWER_SUPPLY_TYPE_WIRELESS;
	case POWER_SUPPLY_CHARGER_TYPE_USB_TYPEC:
		return POWER_SUPPLY_TYPE_USB_TYPEC;
	case POWER_SUPPLY_CHARGER_TYPE_NONE:
	case POWER_SUPPLY_CHARGER_TYPE_USB_SDP:
	default:
		return POWER_SUPPLY_TYPE_USB;
	}

	return POWER_SUPPLY_TYPE_USB;
}
EXPORT_SYMBOL(get_power_supply_type);

static inline void flush_charger_context(struct power_supply *psy)
{
	struct charger_props *chrgr_prop, *tmp;

	list_for_each_entry_safe(chrgr_prop, tmp,
				&psy_chrgr.chrgr_cache_lst, node) {
		if (!strcmp(chrgr_prop->name, psy->name)) {
			list_del(&chrgr_prop->node);
			kfree(chrgr_prop);
		}
	}
}
int power_supply_unregister_charger(struct power_supply *psy)
{
	flush_charger_context(psy);
	return 0;
}
EXPORT_SYMBOL(power_supply_unregister_charger);

int power_supply_register_charging_algo(struct charging_algo *algo)
{
	struct charging_algo *algo_new;

	algo_new = kzalloc(sizeof(*algo_new), GFP_KERNEL);
	if (algo_new == NULL) {
		pr_err("%s: Error allocating memory for algo!!", __func__);
		return -ENOMEM;
	}
	memcpy(algo_new, algo, sizeof(*algo_new));

	list_add_tail(&algo_new->node, &algo_list);
	return 0;
}
EXPORT_SYMBOL(power_supply_register_charging_algo);

int power_supply_unregister_charging_algo(struct charging_algo *algo)
{
	struct charging_algo *algo_l, *tmp;

	list_for_each_entry_safe(algo_l, tmp, &algo_list, node) {
		if (!strcmp(algo_l->name, algo->name)) {
			list_del(&algo_l->node);
			kfree(algo_l);
		}
	}
	return 0;

}
EXPORT_SYMBOL(power_supply_unregister_charging_algo);

static struct charging_algo *get_charging_algo_by_type
		(enum batt_chrg_prof_type chrg_prof_type)
{
	struct charging_algo *algo;

	list_for_each_entry(algo, &algo_list, node) {
		if (algo->chrg_prof_type == chrg_prof_type)
			return algo;
	}

	return NULL;
}

struct charging_algo *power_supply_get_charging_algo
	(struct power_supply *psy, struct ps_batt_chg_prof *batt_prof)
{
	return get_charging_algo_by_type(batt_prof->chrg_prof_type);

}
EXPORT_SYMBOL_GPL(power_supply_get_charging_algo);
fs_initcall(power_supply_charger_init);
