/*
 * Dilemma Max - Akkumessung, die den Spannungseinbruch durch das Underglow herausrechnet.
 *
 * ZMK misst die Akkuspannung am VDDH-Pin (zmk,battery-nrf-vddh) und rechnet sie linear in
 * Prozent um: 3450 mV = 0 %, 4200 mV = 100 %, also 7,5 mV pro Prozent. Gemessen wird nur,
 * solange die Haelfte aktiv ist - und genau dann leuchten die 51 LEDs. Deren Strom laesst
 * die Spannung am Innenwiderstand von Akku, Schalter und Leiterbahnen einbrechen, je nach
 * Helligkeit, Farbe und Effekt unterschiedlich stark. Schon 75 mV sind 10 %. Die Anzeige
 * sprang deshalb mit dem Underglow und fiel bei halb leerem Akku auf 0 %, obwohl die
 * Tastatur noch lange lief.
 *
 * Dieser Treiber setzt sich als zmk,battery vor den VDDH-Sensor und liefert ZMK einen
 * bereinigten Wert:
 *
 *  - Er misst selbst, alle 30 s und unabhaengig vom Activity-State, jeweils den Median aus
 *    fuenf Messungen.
 *  - Jede Messung wird danach einsortiert, ob das Underglow gerade an ist. Liegen eine
 *    Messung mit und eine ohne Underglow direkt hintereinander (Wechsel in den Idle oder
 *    zurueck), ist ihre Differenz der Einbruch. Der wird gemerkt und auf alle Messungen
 *    mit leuchtendem Underglow aufgeschlagen. Er lernt sich also selbst und zieht nach,
 *    wenn Helligkeit oder Effekt geaendert werden.
 *  - Das Ergebnis laeuft durch einen Tiefpass, und der Prozentwert aendert sich erst ab
 *    zwei Prozent Abstand, damit er nicht um eine Stelle zappelt.
 *
 * Ein Abruf durch ZMK loest bewusst keine Messung aus: ZMK fragt genau in dem Moment, in
 * dem die Haelfte aufwacht - da gilt das Underglow schon als an, leuchtet aber noch nicht.
 *
 * 0 % wird nie gemeldet, das Minimum ist 1: der Wert 0 steht im Split-Protokoll und auf
 * dem Display fuer "nicht verbunden".
 */

#define DT_DRV_COMPAT dilemma_battery_filter

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <stdlib.h>

#include <zmk/rgb_underglow.h>
#include <zmk/workqueue.h>

LOG_MODULE_REGISTER(dilemma_max_battery, CONFIG_ZMK_LOG_LEVEL);

#define SAMPLE_PERIOD K_SECONDS(30)
#define SAMPLE_COUNT 5
#define SAMPLE_GAP K_MSEC(2)

// Zwei Messungen gelten als Paar, wenn sie hoechstens so weit auseinander liegen. In der
// Zeit aendert sich der Ladestand praktisch nicht, der Unterschied ist also der Einbruch.
#define SAG_PAIR_MAX_MS (2 * 60 * MSEC_PER_SEC)
// Mehr ist kein Einbruch durch die LEDs mehr, sondern ein gestecktes oder gezogenes Kabel.
#define SAG_MAX_MV 400
// Ab diesem Sprung folgt der Tiefpass sofort (USB gesteckt oder gezogen).
#define FILTER_SNAP_MV 150
#define SOC_HYSTERESIS 2

struct battery_filter_config {
    const struct device *source;
};

struct battery_filter_data {
    struct k_work_delayable sample_work;
    bool started;

    bool have_value;
    int32_t filtered_mv;
    uint8_t state_of_charge;

    bool have_prev;
    bool prev_loaded;
    int32_t prev_mv;
    int64_t prev_ts;

    bool have_sag;
    int32_t sag_mv;
};

static bool underglow_is_on(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    bool on = false;
    return zmk_rgb_underglow_get_state(&on) == 0 && on;
#else
    return false;
#endif
}

// Dieselbe Kennlinie wie ZMK (battery_common.c), nur mit 1 statt 0 als Untergrenze.
static uint8_t mv_to_pct(int32_t mv) {
    if (mv >= 4200) {
        return 100;
    }
    return (uint8_t)CLAMP(mv * 2 / 15 - 459, 1, 100);
}

static int read_source_mv(const struct device *source, int32_t *mv) {
    struct sensor_value voltage;

    int rc = sensor_sample_fetch_chan(source, SENSOR_CHAN_GAUGE_VOLTAGE);
    if (rc != 0) {
        return rc;
    }
    rc = sensor_channel_get(source, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage);
    if (rc != 0) {
        return rc;
    }

    *mv = voltage.val1 * 1000 + voltage.val2 / 1000;
    return 0;
}

static int read_median_mv(const struct device *source, int32_t *mv) {
    int32_t samples[SAMPLE_COUNT];

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        int32_t value;
        int rc = read_source_mv(source, &value);
        if (rc != 0) {
            return rc;
        }

        // Einsortieren, damit am Ende der mittlere Wert in der Mitte steht
        int pos = i;
        while (pos > 0 && samples[pos - 1] > value) {
            samples[pos] = samples[pos - 1];
            pos--;
        }
        samples[pos] = value;

        k_sleep(SAMPLE_GAP);
    }

    *mv = samples[SAMPLE_COUNT / 2];
    return 0;
}

static int battery_filter_sample(const struct device *dev) {
    const struct battery_filter_config *config = dev->config;
    struct battery_filter_data *data = dev->data;

    // Vor und nach der Messung pruefen: schaltet das Underglow mittendrin, ist unklar, was
    // gemessen wurde.
    bool loaded = underglow_is_on();
    int32_t mv;
    int rc = read_median_mv(config->source, &mv);
    if (rc != 0) {
        LOG_WRN("Akkumessung fehlgeschlagen (%d)", rc);
        return rc;
    }
    if (underglow_is_on() != loaded) {
        return 0;
    }

    int64_t now = k_uptime_get();

    if (data->have_prev && data->prev_loaded != loaded && now - data->prev_ts <= SAG_PAIR_MAX_MS) {
        int32_t sag = loaded ? data->prev_mv - mv : mv - data->prev_mv;
        if (sag >= 0 && sag <= SAG_MAX_MV) {
            data->sag_mv = data->have_sag ? (3 * data->sag_mv + sag) / 4 : sag;
            data->have_sag = true;
        }
    }
    data->have_prev = true;
    data->prev_loaded = loaded;
    data->prev_mv = mv;
    data->prev_ts = now;

    int32_t estimate = mv + (loaded ? data->sag_mv : 0);

    if (!data->have_value || abs(estimate - data->filtered_mv) > FILTER_SNAP_MV) {
        data->filtered_mv = estimate;
    } else {
        data->filtered_mv += (estimate - data->filtered_mv) / 4;
    }

    uint8_t pct = mv_to_pct(data->filtered_mv);
    if (!data->have_value || pct == 100 ||
        abs((int)pct - (int)data->state_of_charge) >= SOC_HYSTERESIS) {
        data->state_of_charge = pct;
    }
    data->have_value = true;

    LOG_DBG("Akku %d mV (%s), Einbruch %d mV, gefiltert %d mV => %d%%", mv,
            loaded ? "Underglow an" : "Underglow aus", data->sag_mv, data->filtered_mv,
            data->state_of_charge);

    return 0;
}

static void battery_filter_sample_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct battery_filter_data *data =
        CONTAINER_OF(dwork, struct battery_filter_data, sample_work);

    battery_filter_sample(DEVICE_DT_INST_GET(0));
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &data->sample_work, SAMPLE_PERIOD);
}

static int battery_filter_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    struct battery_filter_data *data = dev->data;

    if (chan != SENSOR_CHAN_GAUGE_VOLTAGE && chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE &&
        chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

    // Die eigene Messschleife startet mit dem ersten Abruf: ZMK ruft aus seiner Low-Prio-
    // Workqueue heraus an, die es beim Init dieses Treibers noch nicht gibt.
    if (!data->started) {
        data->started = true;
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &data->sample_work,
                                  SAMPLE_PERIOD);
    }

    if (!data->have_value) {
        return battery_filter_sample(dev);
    }

    return 0;
}

static int battery_filter_channel_get(const struct device *dev, enum sensor_channel chan,
                                      struct sensor_value *val) {
    const struct battery_filter_data *data = dev->data;

    if (!data->have_value) {
        return -ENODATA;
    }

    switch (chan) {
    case SENSOR_CHAN_GAUGE_VOLTAGE:
        val->val1 = data->filtered_mv / 1000;
        val->val2 = (data->filtered_mv % 1000) * 1000;
        return 0;
    case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
        val->val1 = data->state_of_charge;
        val->val2 = 0;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static const struct sensor_driver_api battery_filter_api = {
    .sample_fetch = battery_filter_sample_fetch,
    .channel_get = battery_filter_channel_get,
};

static int battery_filter_init(const struct device *dev) {
    const struct battery_filter_config *config = dev->config;
    struct battery_filter_data *data = dev->data;

    if (!device_is_ready(config->source)) {
        LOG_ERR("Akkusensor \"%s\" ist nicht bereit", config->source->name);
        return -ENODEV;
    }

    k_work_init_delayable(&data->sample_work, battery_filter_sample_work_cb);
    return 0;
}

static struct battery_filter_data battery_filter_data_0;
static const struct battery_filter_config battery_filter_config_0 = {
    .source = DEVICE_DT_GET(DT_INST_PHANDLE(0, source)),
};

// Gleiche Prioritaet wie der VDDH-Sensor: bei Gleichstand sortiert Zephyr nach den
// Abhaengigkeiten im Devicetree, die Quelle kommt also zuerst dran.
DEVICE_DT_INST_DEFINE(0, battery_filter_init, NULL, &battery_filter_data_0,
                      &battery_filter_config_0, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
                      &battery_filter_api);
