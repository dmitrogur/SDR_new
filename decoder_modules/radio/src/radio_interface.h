#pragma once

enum {
    RADIO_IFACE_CMD_GET_MODE,
    RADIO_IFACE_CMD_SET_MODE,
    RADIO_IFACE_CMD_GET_BANDWIDTH,
    RADIO_IFACE_CMD_SET_BANDWIDTH,
    RADIO_IFACE_CMD_GET_SQUELCH_ENABLED,
    RADIO_IFACE_CMD_SET_SQUELCH_ENABLED,
    RADIO_IFACE_CMD_GET_SQUELCH_LEVEL,
    RADIO_IFACE_CMD_SET_SQUELCH_LEVEL,
    RADIO_IFACE_CMD_SET_SNAPINTERVAL,

    RADIO_IFACE_CMD_ADD_TO_IFCHAIN,
    RADIO_IFACE_CMD_REMOVE_FROM_IFCHAIN,
    RADIO_IFACE_CMD_ENABLE_IN_IFCHAIN,
    RADIO_IFACE_CMD_DISABLE_IN_IFCHAIN,

    RADIO_IFACE_CMD_ADD_TO_AFCHAIN,
    RADIO_IFACE_CMD_REMOVE_FROM_AFCHAIN,
    RADIO_IFACE_CMD_ENABLE_IN_AFCHAIN,
    RADIO_IFACE_CMD_DISABLE_IN_AFCHAIN
};

enum {
    RADIO_IFACE_MODE_NFM,
    RADIO_IFACE_MODE_WFM,
    RADIO_IFACE_MODE_AM,
    RADIO_IFACE_MODE_DSB,
    RADIO_IFACE_MODE_USB,
    RADIO_IFACE_MODE_CW,
    RADIO_IFACE_MODE_LSB,
    RADIO_IFACE_MODE_RAW,
    RADIO_IFACE_MODE_IQ
};

enum RadioInterfaceCommand {
    // ...
    RADIO_IFACE_CMD_BIND_REMOTE_STREAM,
    RADIO_IFACE_CMD_UNBIND_REMOTE_STREAM
};

const char* demodModeListEngl[] = {
    "NFM",
    "WFM",
    "AM",
    "DSB",
    "USB",
    "CW",
    "LSB",
    "RAW"
};

const char* demodModeList[] = {
    "ЧМ",
    "ЧМ-Ш",
    "AM",
    "ПБС",
    "ВБС",
    "HC",
    "НБС",
    "CMO"
};

const char* demodModeListFile[] = {
    "ЧМ",
    "ЧМ-Ш",
    "AM",
    "ПБС",
    "ВБС",
    "HC",
    "НБС",
    "CMO"
};

const char* demodModeListTxtEngl = "NFM\0WFM\0AM\0DSB\0USB\0CW\0LSB\0RAW\0";
const char* demodModeListTxt     = "ЧМ\0ЧМ-Ш\0AM\0ПБС\0ВБС\0HC\0НБС\0CMO\0"; // IQ\0
const char* SignalListTxt     = "Авто\0ТЛФ\0DMR\0";

//const char* bandListTxt = " 1000\0 6250\0 12500\0 25000\0 50000\0 100000\0 220000\0";
const char* bandListTxt = " 1\0 2.1\0 4\0 6.25\0 12.5\0 25\0 50\0 100\0 250\0";
