set(COMPONENT_SRCS
    ./main.c
    ./../frwk/es8311.c
    ./../frwk/esp_websocket_client.c
    ./src_wifi.c
)
set(COMPONENT_ADD_INCLUDEDIRS
    "./include"
    "./../frwk/include"
)
set(COMPONENT_EMBED_TXTFILES "../extern_files/rcv.pcm")
# spiffs_create_partition_image(model ../model FLASH_IN_PROJECT)
register_component()
