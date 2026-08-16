# Projekt-CMakeLists fuer den reinen ESP-IDF-Build.
# Wird von PlatformIO bei framework = espidf erwartet.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_wifi_bridge)

# NBSTAT-Antworten in lwIPs NetBIOS-Responder einschalten.
#
# Ohne das beantwortet die Bridge nur Abfragen nach ihrem exakt bekannten
# Namen - ein Router kann den aber nicht kennen, bevor er ihn erfragt hat.
# NBSTAT ("Node Status") ist genau die Abfrage, mit der Namen ENTDECKT werden;
# so lernt die FritzBox die Namen von Windows-Rechnern.
#
# lwIP setzt LWIP_NETBIOS_RESPOND_NAME_QUERY per Vorgabe auf 0, und ESP-IDF
# bietet keine Kconfig-Option dafuer. Deshalb hier direkt an der lwip-
# Komponente definiert - der von ESP-IDF vorgesehene Weg, eine Fremdkomponente
# zu parametrisieren, ohne sie zu patchen.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${lwip_lib} PRIVATE LWIP_NETBIOS_RESPOND_NAME_QUERY=1)
