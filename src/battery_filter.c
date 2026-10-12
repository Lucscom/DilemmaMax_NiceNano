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
 *  - Der Prozentwert aendert sich erst ab zwei Prozent Abstand, damit er nicht um eine
 *    Stelle zappelt. Die Spannung selbst wird nicht weiter geglaettet.
 *
 * Ein Abruf durch ZMK loest bewusst keine Messung aus: ZMK fragt genau in dem Moment, in
 * dem die Haelfte aufwacht - da gilt das Underglow schon als an, leuchtet aber noch nicht.
 *
 * 0 % wird nie gemeldet, das Minimum ist 1: der Wert 0 steht im Split-Protokoll und auf
 * dem Display fuer "nicht verbunden".
 *
 * Am USB-Kabel gilt das alles nicht: dort misst VDDH den Ausgang des Ladechips, und der
 * Ladestand kommt aus einer eigenen Kennlinie, siehe battery_filter_charging().
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
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif
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
#define SOC_HYSTERESIS 2
// Am USB-Kabel haelt der Ladechip VDDH etwa 225 mV ueber der Zelle, hoechstens aber auf
// 4,4 V. Ab hier ist diese Grenze erreicht und die Spannung sagt nichts mehr.
#define CHARGE_PLATEAU_MV 4375
// Ladestand, bei dem die Grenze erreicht wird, und wie lange der Rest danach noch dauert
#define CHARGE_PLATEAU_PCT 91
#define CHARGE_TAIL_MS (30 * 60 * MSEC_PER_SEC)

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
    bool charging;
    int64_t plateau_ts; // seit wann VDDH an der 4,4-V-Grenze steht, 0 = noch nicht
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
 * Kennlinien, Stuetzstellen in mV und Prozent, dazwischen linear. Beide stammen aus einem
 * aufgezeichneten Ladevorgang der linken Haelfte von leer bis voll (220-mAh-Zelle, 100 mA
 * Ladestrom, Underglow aus).
 *
 * Was dabei herauskam: am USB-Kabel liegt an VDDH nicht die Zelle, sondern der Ausgang des
 * Ladechips. Der laeuft etwa 225 mV ueber der Zelle mit, von 3,4 V bis hoechstens 4,4 V.
 * Die Spannung stieg knapp 120 Minuten lang gleichmaessig und blieb dann auf 4385 mV
 * stehen. Bis dahin laedt der Chip mit konstantem Strom, die Zeit ist also der Ladestand:
 * 120 min x 100 mA = 200 mAh, 91 % der Zelle.
 *
 * charge_curve ist genau diese Messung: VDDH am Kabel -> Ladestand.
 *
 * discharge_curve ist daraus abgeleitet, nicht gemessen: dieselben Ladestaende, 275 mV
 * tiefer (225 mV Abstand des Ladechips, 50 mV, die eine Zelle beim Laden ueber ihrer
 * Ruhespannung liegt). Der oberste Punkt ist gemessen: eine volle Zelle zeigte direkt nach
 * dem Abziehen 4140 mV. Unten ist die Kurve am unsichersten.
 */
struct curve_point {
    int16_t mv;
    uint8_t pct;
};

static const struct curve_point charge_curve[] = {
    {4380, 91}, {4330, 87}, {4280, 82}, {4235, 77}, {4200, 73}, {4170, 68}, {4140, 64},
    {4110, 59}, {4085, 55}, {4055, 45}, {4035, 34}, {4000, 27}, {3960, 23}, {3910, 18},
    {3850, 14}, {3800, 9},  {3710, 5},  {3400, 1},
};

static const struct curve_point discharge_curve[] = {
    {4140, 100}, {4105, 91}, {4055, 87}, {4005, 82}, {3960, 77}, {3925, 73}, {3895, 68},
    {3865, 64},  {3835, 59}, {3810, 55}, {3780, 45}, {3760, 34}, {3725, 27}, {3685, 23},
    {3635, 18},  {3575, 14}, {3525, 9},  {3435, 5},  {3300, 1},
};

// Unten endet jede Kurve bei 1 statt 0, siehe oben
static uint8_t curve_pct(const struct curve_point *curve, size_t len, int32_t mv) {
    if (mv >= curve[0].mv) {
        return curve[0].pct;
    }

    for (size_t i = 1; i < len; i++) {
        if (mv >= curve[i].mv) {
            int32_t span_mv = curve[i - 1].mv - curve[i].mv;
            int32_t span_pct = curve[i - 1].pct - curve[i].pct;
            return curve[i].pct + (mv - curve[i].mv) * span_pct / span_mv;
        }
    }

    return 1;
}

static uint8_t mv_to_pct(int32_t mv) {
    return curve_pct(discharge_curve, ARRAY_SIZE(discharge_curve), mv);
}

// Steckt das USB-Kabel? Links weiss das ZMK, rechts die VBUS-Erkennung des nRF52840.
static bool usb_powered(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#elif IS_ENABLED(CONFIG_DILEMMA_MAX_PERIPHERAL_CHARGE_REPORT)
    return dilemma_max_usb_powered();
#else
    return false;
#endif
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
 * Laden am USB-Kabel. Der Ladestand kommt aus charge_curve, solange VDDH noch steigt.
 * Steht VDDH an der 4,4-V-Grenze, fehlen der Zelle noch die letzten rund 9 %, und die
 * Spannung verraet nicht mehr, wie weit sie ist: von da an zaehlt die Zeit, 30 Minuten bis
 * 100 %. Ob der Ladechip wirklich fertig ist, zeigt nur seine LED.
 *
 * Waehrend eines Ladevorgangs faellt der Wert nie: das Underglow zieht VDDH etwas herunter,
 * und die Anzeige soll deshalb nicht rueckwaerts laufen.
 */
static void battery_filter_charging(struct battery_filter_data *data, int32_t mv, int64_t now) {
    bool started = !data->charging;

    data->filtered_mv = mv;

    uint8_t pct;
    if (data->filtered_mv >= CHARGE_PLATEAU_MV) {
        if (data->plateau_ts == 0) {
            data->plateau_ts = MAX(now, 1);
        }
        int64_t tail = MIN(now - data->plateau_ts, CHARGE_TAIL_MS);
        pct = CHARGE_PLATEAU_PCT + (100 - CHARGE_PLATEAU_PCT) * tail / CHARGE_TAIL_MS;
    } else {
        data->plateau_ts = 0;
        pct = curve_pct(charge_curve, ARRAY_SIZE(charge_curve), data->filtered_mv);
    }

    // 1 liest das Display beim Laden als "Stand unbekannt"
    pct = MAX(pct, 2);
    data->state_of_charge = started ? pct : MAX(data->state_of_charge, pct);

    data->charging = true;
    data->have_value = true;
    // Als Paar fuer den Einbruch taugt die Messung nicht
    data->have_prev = false;

    LOG_INF("Akku laedt: VDDH %d mV => %d%%", mv, data->state_of_charge);
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

    if (usb_powered()) {
        battery_filter_charging(data, mv, now);
        return 0;
    }
    if (data->charging) {
        // Kabel ab: die Messung zeigt wieder die Zelle und liegt gut 200 mV tiefer. Von vorn
        // anfangen, ohne Hysterese zum Wert aus der Ladekurve.
        data->charging = false;
        data->plateau_ts = 0;
        data->have_value = false;
    }

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

    // Kein Tiefpass: der Median aus fuenf Messungen ist ruhig genug, und ein Tiefpass
    // hinkte der Zelle im aufgezeichneten Ladevorgang sichtbar hinterher.
    data->filtered_mv = estimate;

    uint8_t pct = mv_to_pct(data->filtered_mv);
    if (!data->have_value || pct == 100 ||
        abs((int)pct - (int)data->state_of_charge) >= SOC_HYSTERESIS) {
        data->state_of_charge = pct;
    }
    data->have_value = true;

    LOG_INF("Akku %d mV (%s), Einbruch %d mV, bereinigt %d mV => %d%%", mv,
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
