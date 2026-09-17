# Dilemma Max - eigene Quellen fuer den ZMK-Build.
#
# Das Repo-Root wird ueber ZMK_EXTRA_MODULES als Zephyr-Modul eingebunden (siehe
# zephyr/module.yml). Die Quellen gehen direkt an das ZMK-Target "app", weil nur
# dieses die ZMK-Header unter zmk/app/include im Include-Pfad hat.

target_sources_ifdef(CONFIG_DILEMMA_MAX_RGB_SPLIT_SYNC app PRIVATE src/rgb_split_sync.c)
target_sources_ifdef(CONFIG_DILEMMA_MAX_RGB_SLEEP_BLANK app PRIVATE src/rgb_sleep_blank.c)
