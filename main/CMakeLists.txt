idf_component_register(SRCS main.c img.c)

#设置 LittleFS 文件夹路径
# set(LITTLEFS_DIR "${CMAKE_CURRENT_LIST_DIR}/../littlefs")
 
# 生成 LittleFS 分区镜像
#  idf_build_get_property(idf_target IDF_TARGET)
#  littlefs_create_partition_image(littlefs "${LITTLEFS_DIR}" FLASH_IN_PROJECT)