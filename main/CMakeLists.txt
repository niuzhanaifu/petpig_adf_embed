set(COMPONENT_SRCS ./main.c ./esp_websocket_client.c ./es8311.c ./src_wifi.c ./esp_websocket_client.c)
set(COMPONENT_ADD_INCLUDEDIRS "./include")
set(COMPONENT_EMBED_TXTFILES "../extern_files/rcv.pcm")
register_component()
