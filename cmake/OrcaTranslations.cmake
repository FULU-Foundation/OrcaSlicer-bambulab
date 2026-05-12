cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED ORCA_SOURCE_DIR)
    get_filename_component(ORCA_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif ()

if (NOT DEFINED ORCA_TRANSLATION_FULL)
    set(ORCA_TRANSLATION_FULL OFF)
endif ()

set(ORCA_RESOURCES_DIR "${ORCA_SOURCE_DIR}/resources")
set(ORCA_L10N_DIR "${ORCA_SOURCE_DIR}/localization/i18n")
set(ORCA_POT_FILE "${ORCA_L10N_DIR}/OrcaSlicer.pot")

find_program(GETTEXT_MSGFMT_EXECUTABLE msgfmt REQUIRED)

if (ORCA_TRANSLATION_FULL)
    find_program(GETTEXT_XGETTEXT_EXECUTABLE xgettext REQUIRED)
    find_program(GETTEXT_MSGMERGE_EXECUTABLE msgmerge REQUIRED)
    find_program(PYTHON_EXECUTABLE NAMES python3 python REQUIRED)

    execute_process(
        COMMAND "${GETTEXT_XGETTEXT_EXECUTABLE}"
            --keyword=L
            --keyword=_L
            --keyword=_u8L
            --keyword=L_CONTEXT:1,2c
            --keyword=_L_PLURAL:1,2
            --add-comments=TRN
            --from-code=UTF-8
            --no-location
            --debug
            --boost
            -f "${ORCA_L10N_DIR}/list.txt"
            -o "${ORCA_POT_FILE}"
        WORKING_DIRECTORY "${ORCA_SOURCE_DIR}"
        RESULT_VARIABLE gettext_result
    )
    if (NOT gettext_result EQUAL 0)
        message(FATAL_ERROR "xgettext failed with exit code ${gettext_result}")
    endif ()

    execute_process(
        COMMAND "${PYTHON_EXECUTABLE}" "${ORCA_SOURCE_DIR}/scripts/HintsToPot.py" "${ORCA_RESOURCES_DIR}" "${ORCA_L10N_DIR}"
        WORKING_DIRECTORY "${ORCA_SOURCE_DIR}"
        RESULT_VARIABLE hints_result
    )
    if (NOT hints_result EQUAL 0)
        message(FATAL_ERROR "HintsToPot.py failed with exit code ${hints_result}")
    endif ()
endif ()

file(GLOB translation_dirs LIST_DIRECTORIES true "${ORCA_L10N_DIR}/*")
foreach(translation_dir IN LISTS translation_dirs)
    if (NOT IS_DIRECTORY "${translation_dir}")
        continue()
    endif ()

    get_filename_component(lang "${translation_dir}" NAME)
    set(po_file "${translation_dir}/OrcaSlicer_${lang}.po")

    if (EXISTS "${po_file}")
        if (ORCA_TRANSLATION_FULL)
            execute_process(
                COMMAND "${GETTEXT_MSGMERGE_EXECUTABLE}" -N -o "${po_file}" "${po_file}" "${ORCA_POT_FILE}"
                WORKING_DIRECTORY "${ORCA_SOURCE_DIR}"
                RESULT_VARIABLE merge_result
            )
            if (NOT merge_result EQUAL 0)
                message(FATAL_ERROR "msgmerge failed for ${lang} with exit code ${merge_result}")
            endif ()
        endif ()

        set(mo_dir "${ORCA_RESOURCES_DIR}/i18n/${lang}")
        file(MAKE_DIRECTORY "${mo_dir}")

        execute_process(
            COMMAND "${GETTEXT_MSGFMT_EXECUTABLE}" --check-format -o "${mo_dir}/OrcaSlicer.mo" "${po_file}"
            WORKING_DIRECTORY "${ORCA_SOURCE_DIR}"
            RESULT_VARIABLE msgfmt_result
        )
        if (NOT msgfmt_result EQUAL 0)
            message(FATAL_ERROR "msgfmt failed for ${lang} with exit code ${msgfmt_result}")
        endif ()
    endif ()
endforeach()
