idf_component_register(
  SRCS 
  "audio_bsp.c"
  PRIV_REQUIRES 
  main
  codec_board
  esp_codec_dev
  INCLUDE_DIRS "./"
)
