if(CONFIG_ZEPHLETS_COAP)
    zephyr_library_named(zephlet_coap_frontend)
    zephyr_library_sources(
        zephlet_coap_frontend.c
        zephlet_coap_translate.c
        zephlet_coap_send.c
        zephlet_coap_uri.c
        zephlet_coap_discovery.c
    )
    zephyr_include_directories(include)

    # `COAP_SERVICE_DEFINE(zlet_coap_service, ...)` plus each codegen-emitted
    # `COAP_RESOURCE_DEFINE(<type>_coap_resource, zlet_coap_service, ...)`
    # produces references to `_coap_resource_zlet_coap_service_list_{start,end}`.
    # Declare the matching iterable section here so consuming apps don't need
    # to replicate the linker glue.
    zephyr_linker_sources(DATA_SECTIONS sections-ram.ld)
    zephyr_iterable_section(
        NAME coap_resource_zlet_coap_service
        GROUP DATA_REGION ${XIP_ALIGN_WITH_INPUT})
endif()
