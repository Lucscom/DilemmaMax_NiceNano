# Dilemma Max - eigene Quellen fuer den ZMK-Build.
#
# Das Repo-Root wird ueber ZMK_EXTRA_MODULES als Zephyr-Modul eingebunden (siehe
# zephyr/module.yml). Die Quellen gehen direkt an das ZMK-Target "app", weil nur
# dieses die ZMK-Header unter zmk/app/include im Include-Pfad hat.

# maXTouch-Trackpadtreiber. Frueher ein eigenes Repo (philippkober/maxtouch-zephyr-module);
# da praktisch der ganze Treiber ohnehin hier entstanden ist und upstream nichts liefert,
# was wir nicht haben, liegt er jetzt direkt im Config-Repo. Er geht als Zephyr-Treiber
# (zephyr_library_amend) in die Input-Bibliothek, nicht an das ZMK-Target "app".
add_subdirectory(drivers)

target_sources_ifdef(CONFIG_DILEMMA_MAX_RGB_SPLIT_SYNC app PRIVATE src/rgb_split_sync.c)
target_sources_ifdef(CONFIG_DILEMMA_MAX_RGB_SPLIT_FOLLOW app PRIVATE src/rgb_split_follow.c)
target_sources_ifdef(CONFIG_DILEMMA_MAX_RGB_SLEEP_BLANK app PRIVATE src/rgb_sleep_blank.c)
target_sources_ifdef(CONFIG_DILEMMA_MAX_STATUS_SCREEN app PRIVATE src/status_screen.c)
target_sources_ifdef(CONFIG_DILEMMA_MAX_PERIPHERAL_CHARGE_REPORT app PRIVATE src/peripheral_charge_report.c)
