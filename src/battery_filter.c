/*
 * Dilemma Max - Akkumessung, die den Spannungseinbruch durch das Underglow herausrechnet.
 *
 * ZMK misst die Akkuspannung am VDDH-Pin (zmk,battery-nrf-vddh) und rechnet sie linear in
 * Prozent um: 3450 mV = 0 %, 4200 mV = 100 %, also 7,5 mV pro Prozent. (Die Umrechnung
 * macht inzwischen mv_to_pct() weiter unten mit einer eigenen Kurve.) Gemessen wird nur,
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

#include <dilemma_max/battery.h>
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
// Mehr liefert keine LiPo-Zelle, das ist die USB-Versorgung
#define EXTERNAL_POWER_MV 4250
// Ab hier nimmt der Ladestrom ab, die Schaetzung zaehlt die Zeit nur noch halb
#define CHARGE_TAPER_PCT 80
#define CHARGE_MAX_PCT 99

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

    // Laden am USB-Kabel, siehe battery_filter_charging()
    bool external;
    bool charge_known;
    int32_t charge_mpct;
    int64_t charge_ts;
};

static bool underglow_is_on(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    bool on = false;
    return zmk_rgb_underglow_get_state(&on) == 0 && on;
#else
    return false;
#endif
}

/*
 * Entladekurve einer LiPo-Zelle in Ruhe, Stuetzstellen in mV und Prozent, dazwischen linear.
 *
 * ZMK rechnet mit einer Geraden von 3450 mV (0 %) bis 4200 mV (100 %). Die passt an beiden
 * Enden nicht: 4200 mV liegen nur an, solange das Ladegeraet drueckt - gemessen wurden an
 * einer vollen Zelle direkt nach dem Abziehen 4140 mV, das waren 93 %. Und in der flachen
 * Mitte der Kurve zeigt die Gerade bei 3700 mV noch ein Drittel an, wo real nur noch ein
 * Fuenftel uebrig ist.
 *
 * Die Werte sind eine typische Kurve, nicht an diesen Zellen ausgemessen. Unten endet sie
 * bei 1 statt 0, siehe oben.
 */
static const struct {
    int16_t mv;
    uint8_t pct;
} discharge_curve[] = {
    {4120, 100}, {4050, 90}, {3980, 80}, {3920, 70}, {3870, 60}, {3820, 50},
    {3790, 40},  {3770, 30}, {3740, 20}, {3680, 10}, {3600, 5},  {3450, 1},
};

static uint8_t mv_to_pct(int32_t mv) {
    if (mv >= discharge_curve[0].mv) {
        return 100;
    }

    for (size_t i = 1; i < ARRAY_SIZE(discharge_curve); i++) {
        if (mv >= discharge_curve[i].mv) {
            int32_t span_mv = discharge_curve[i - 1].mv - discharge_curve[i].mv;
            int32_t span_pct = discharge_curve[i - 1].pct - discharge_curve[i].pct;
            return discharge_curve[i].pct + (mv - discharge_curve[i].mv) * span_pct / span_mv;
        }
    }

    return 1;
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

/*
 * Am USB-Kabel liegt an VDDH nicht der Akku an, sondern die Versorgung: gemessen wurden
 * konstant 4385 bis 4390 mV, egal wie voll die Zelle war. Der Ladechip des nice!nano meldet
 * dem Controller auch nicht, wie weit er ist. Messen laesst sich der Ladestand beim Laden
 * also nicht, nur schaetzen:
 *
 *   Stand beim Einstecken + Ladestrom x Zeit / Kapazitaet
 *
 * Ab 80 % zaehlt die Zeit nur noch halb, weil der Ladestrom dort abnimmt, und bei 99 ist
 * Schluss - ob die Zelle wirklich voll ist, zeigt nur die Lade-LED. Nach dem Abziehen wird
 * wieder gemessen, und der Tiefpass springt auf den echten Wert.
 *
 * Startet die Haelfte am Kabel (nach dem Flashen, oder per Schalter eingeschaltet), fehlt
 * der Stand beim Einstecken. Dann wird 1 gemeldet, was das Display als "laedt, Stand
 * unbekannt" ohne Zahl zeigt.
 */
static void battery_filter_charging(struct battery_filter_data *data, int32_t mv, int64_t now) {
    if (!data->external) {
        data->external = true;
        data->charge_known = data->have_value;
        data->charge_mpct = data->state_of_charge * 1000;
    } else if (data->charge_known) {
        // Promille-Prozent: dt [ms] * I [mA] * 100000 / (C [mAh] * 3600000)
        int32_t gain = (int32_t)(now - data->charge_ts) * CONFIG_DILEMMA_MAX_CHARGE_CURRENT_MA /
                       (CONFIG_DILEMMA_MAX_BATTERY_CAPACITY_MAH * 36);
        if (data->charge_mpct >= CHARGE_TAPER_PCT * 1000) {
            gain /= 2;
        }
        data->charge_mpct = MIN(data->charge_mpct + gain, CHARGE_MAX_PCT * 1000);
    }
    data->charge_ts = now;

    // Als Paar fuer den Einbruch taugt die Messung nicht
    data->have_prev = false;
    data->filtered_mv = mv;
    data->state_of_charge = data->charge_known ? CLAMP(data->charge_mpct / 1000, 2, 99) : 1;
    data->have_value = true;

    LOG_INF("Akku %d mV: externe Versorgung, Ladestand geschaetzt %d%%", mv,
            data->state_of_charge);
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

    if (mv > EXTERNAL_POWER_MV) {
        battery_filter_charging(data, mv, now);
        return 0;
    }
    data->external = false;

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

    LOG_INF("Akku %d mV (%s), Einbruch %d mV, gefiltert %d mV => %d%%", mv,
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
        // Auf der rechten Haelfte bekommt schon ZMK den kodierten Wert. Sonst schriebe es
        // bei jeder Messung den rohen in den Battery Service, und ein ungerader Rohwert
        // stuende auf dem Display als "laedt", bis ihn die Kodierung wieder ueberschreibt.
        val->val1 = IS_ENABLED(CONFIG_DILEMMA_MAX_PERIPHERAL_CHARGE_REPORT)
                        ? dilemma_max_charge_encode(data->state_of_charge)
                        : data->state_of_charge;
        val->val2 = 0;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static struct battery_filter_data battery_filter_data_0;

uint8_t dilemma_max_battery_filter_level(void) {
    return battery_filter_data_0.have_value ? battery_filter_data_0.state_of_charge : 0;
}

int32_t dilemma_max_battery_filter_mv(void) {
    return battery_filter_data_0.have_value ? battery_filter_data_0.filtered_mv : 0;
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

static const struct battery_filter_config battery_filter_config_0 = {
    .source = DEVICE_DT_GET(DT_INST_PHANDLE(0, source)),
};

// Gleiche Prioritaet wie der VDDH-Sensor: bei Gleichstand sortiert Zephyr nach den
// Abhaengigkeiten im Devicetree, die Quelle kommt also zuerst dran.
DEVICE_DT_INST_DEFINE(0, battery_filter_init, NULL, &battery_filter_data_0,
                      &battery_filter_config_0, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
                      &battery_filter_api);
