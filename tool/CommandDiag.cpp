/*****************************************************************************
 *
 *  Copyright (C) 2026  Sascha Ittner <sascha.ittner@modusoft.de>
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 ****************************************************************************/

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
using namespace std;

#include "CommandDiag.h"
#include "MasterDevice.h"

/*****************************************************************************
 * ETG.1020 diagnosis history, object 0x10F3:
 *
 *   :01  maximum messages          :04  new messages available
 *   :02  newest message subindex   :05  flags
 *   :03  newest acknowledged       :06.. message ring buffer
 *
 * Each message is  DiagCode(u32) Flags(u16) TextId(u16) Timestamp(u64)
 * followed by parameters, each a CoE data type index (u16) and its payload.
 ****************************************************************************/

#define DIAG_HISTORY_INDEX      0x10f3
#define DIAG_SUB_MAX_MESSAGES   0x01
#define DIAG_SUB_NEWEST         0x02
#define DIAG_SUB_FIRST_MESSAGE  0x06

#define DIAG_HEADER_SIZE        16

/****************************************************************************/

CommandDiag::CommandDiag():
    Command("diag", "Show the diagnosis history of a slave.")
{
}

/****************************************************************************/

string CommandDiag::helpString(const string &binaryBaseName) const
{
    stringstream str;

    str << binaryBaseName << " " << getName() << " [OPTIONS] [ESIFILE]" << endl
        << endl
        << getBriefDescription() << endl
        << endl
        << "Reads the ETG.1020 diagnosis history (object 0x10F3) of a" << endl
        << "slave and prints the stored messages, oldest first. Slaves" << endl
        << "that report diagnosis messages as CoE emergencies with a" << endl
        << "generic error code carry the message identity in the text" << endl
        << "ID, so this is usually the only way to find out what an" << endl
        << "emergency actually meant." << endl
        << endl
        << "Timestamps are as reported by the slave, typically its" << endl
        << "local time since power-on." << endl
        << endl
        << "If ESIFILE is given, it is scanned for <DiagMessage>" << endl
        << "entries and the message texts are resolved. This is a" << endl
        << "best-effort text scan, not a validating XML parse; if a" << endl
        << "text ID is not found the numeric ID is shown instead." << endl
        << "Placeholders in the text are left as they are, with the" << endl
        << "decoded parameters printed after it." << endl
        << endl
        << "This command requires a single slave to be selected." << endl
        << endl
        << "Arguments:" << endl
        << "  ESIFILE  is the path of the slave's ESI/XML device" << endl
        << "           description, used to resolve text IDs." << endl
        << endl
        << "Command-specific options:" << endl
        << "  --alias    -a <alias>" << endl
        << "  --position -p <pos>    Slave selection. See the help of" << endl
        << "                         the 'slaves' command." << endl
        << endl
        << numericInfo();

    return str.str();
}

/****************************************************************************/

const char *CommandDiag::typeName(uint16_t flags)
{
    switch (flags & 0x000f) {
        case 0x00: return "Info";
        case 0x01: return "Warning";
        case 0x02: return "Error";
        default:   return "?";
    }
}

/****************************************************************************/

/** Reads a single-byte subindex of the diagnosis history object.
 */
unsigned int CommandDiag::readSubIndexU8(MasterDevice &m,
        uint16_t slavePosition, uint8_t subIndex)
{
    ec_ioctl_slave_sdo_upload_t data;
    unsigned int value;

    data.slave_position = slavePosition;
    data.sdo_index = DIAG_HISTORY_INDEX;
    data.sdo_entry_subindex = subIndex;
    data.target_size = 1;
    data.target = new uint8_t[data.target_size + 1];

    try {
        m.sdoUpload(&data);
    } catch (MasterDeviceException &) {
        delete [] data.target;
        throw;
    }

    value = data.data_size ? data.target[0] : 0;
    delete [] data.target;
    return value;
}

/****************************************************************************/

/** Reads and decodes one message slot.
 *
 * \return true if the slot held a message, false if it was empty or too short.
 */
bool CommandDiag::readMessage(MasterDevice &m, uint16_t slavePosition,
        uint8_t subIndex, Message &msg)
{
    ec_ioctl_slave_sdo_upload_t data;
    bool ok = false;

    data.slave_position = slavePosition;
    data.sdo_index = DIAG_HISTORY_INDEX;
    data.sdo_entry_subindex = subIndex;
    data.target_size = MaxMessageSize;
    data.target = new uint8_t[data.target_size + 1];

    try {
        m.sdoUpload(&data);
    } catch (MasterDeviceSdoAbortException &) {
        // an unused slot may simply not exist
        delete [] data.target;
        return false;
    } catch (MasterDeviceException &) {
        delete [] data.target;
        throw;
    }

    if (data.data_size >= DIAG_HEADER_SIZE) {
        msg.diagCode = EC_READ_U32(data.target);
        msg.flags = EC_READ_U16(data.target + 4);
        msg.textId = EC_READ_U16(data.target + 6);
        msg.timestamp = EC_READ_U64(data.target + 8);
        msg.params.assign((const char *) data.target + DIAG_HEADER_SIZE,
                data.data_size - DIAG_HEADER_SIZE);
        // an all-zero slot has never been written
        ok = msg.diagCode || msg.flags || msg.textId || msg.timestamp;
    }

    delete [] data.target;
    return ok;
}

/****************************************************************************/

/** Decodes the parameter block.
 *
 * Each parameter is a CoE data type index followed by its payload. Only the
 * fixed-width numeric types are decoded; anything else stops the decode and
 * the remainder is dumped as hex, so an unknown encoding cannot be
 * misreported as a value.
 */
string CommandDiag::formatParams(const string &params)
{
    stringstream str;
    size_t pos = 0;
    bool first = true;

    while (pos + 2 <= params.size()) {
        const uint8_t *p = (const uint8_t *) params.data() + pos;
        uint16_t type = EC_READ_U16(p);
        size_t size;
        bool sign;

        switch (type) {
            case 0x0002: size = 1; sign = true;  break; // INTEGER8
            case 0x0003: size = 2; sign = true;  break; // INTEGER16
            case 0x0004: size = 4; sign = true;  break; // INTEGER32
            case 0x0005: size = 1; sign = false; break; // UNSIGNED8
            case 0x0006: size = 2; sign = false; break; // UNSIGNED16
            case 0x0007: size = 4; sign = false; break; // UNSIGNED32
            default:     size = 0; sign = false; break;
        }

        if (!size || pos + 2 + size > params.size()) {
            break;
        }
        pos += 2;
        p = (const uint8_t *) params.data() + pos;

        if (!first) {
            str << ", ";
        }
        first = false;

        if (sign) {
            switch (size) {
                case 1: str << (int) (int8_t) EC_READ_U8(p); break;
                case 2: str << (int) (int16_t) EC_READ_U16(p); break;
                default: str << (int32_t) EC_READ_U32(p); break;
            }
        } else {
            switch (size) {
                case 1: str << (unsigned int) EC_READ_U8(p); break;
                case 2: str << (unsigned int) EC_READ_U16(p); break;
                default: str << (uint32_t) EC_READ_U32(p); break;
            }
        }
        pos += size;
    }

    // anything not understood is shown verbatim rather than guessed at
    if (pos < params.size()) {
        bool trailing = false;
        for (size_t i = pos; i < params.size(); i++) {
            if ((uint8_t) params[i]) {
                trailing = true;
                break;
            }
        }
        if (trailing) {
            if (!first) {
                str << ", ";
            }
            str << "raw";
            for (size_t i = pos; i < params.size(); i++) {
                str << " " << hex << setfill('0') << setw(2)
                    << (unsigned int) (uint8_t) params[i] << dec;
            }
        }
    }

    return str.str();
}

/****************************************************************************/

/** Scans an ESI file for <DiagMessage> text IDs and their English texts.
 *
 * Deliberately a plain text scan: the tool has no XML parser, and the tag
 * shape used by device descriptions is simple and stable enough for this.
 */
void CommandDiag::loadEsiTexts(const string &path, TextMap &texts) const
{
    ifstream file(path.c_str());
    stringstream buf;
    string content;
    size_t pos = 0;

    if (!file.good()) {
        stringstream err;
        err << "Failed to open ESI file '" << path << "'!";
        throwCommandException(err);
    }

    buf << file.rdbuf();
    content = buf.str();

    while ((pos = content.find("<DiagMessage>", pos)) != string::npos) {
        size_t end = content.find("</DiagMessage>", pos);
        if (end == string::npos) {
            break;
        }

        string block = content.substr(pos, end - pos);
        pos = end;

        size_t idPos = block.find("<TextId>");
        if (idPos == string::npos) {
            continue;
        }
        idPos += 8;
        size_t idEnd = block.find("</TextId>", idPos);
        if (idEnd == string::npos) {
            continue;
        }
        string idStr = block.substr(idPos, idEnd - idPos);
        // device descriptions write these as "#x3421"
        if (idStr.size() > 2 && idStr[0] == '#' && (idStr[1] | 0x20) == 'x') {
            idStr = "0x" + idStr.substr(2);
        }

        stringstream idBuf;
        unsigned int id;
        idBuf << idStr;
        idBuf >> resetiosflags(ios::basefield) >> id;
        if (idBuf.fail() || id > 0xffff) {
            continue;
        }

        // prefer the English text, fall back to the first one present
        size_t txtPos = block.find("<MessageText LcId=\"1033\">");
        size_t skip = 25;
        if (txtPos == string::npos) {
            txtPos = block.find("<MessageText");
            if (txtPos == string::npos) {
                continue;
            }
            txtPos = block.find('>', txtPos);
            if (txtPos == string::npos) {
                continue;
            }
            skip = 1;
        }
        txtPos += skip;
        size_t txtEnd = block.find("</MessageText>", txtPos);
        if (txtEnd == string::npos) {
            continue;
        }

        texts[(uint16_t) id] = block.substr(txtPos, txtEnd - txtPos);
    }
}

/****************************************************************************/

void CommandDiag::execute(const StringVector &args)
{
    SlaveList slaves;
    stringstream err;
    TextMap texts;
    unsigned int maxMessages = 0, newest = 0, shown = 0;
    uint16_t slavePosition;

    if (args.size() > 1) {
        err << "'" << getName() << "' takes at most one argument!";
        throwInvalidUsageException(err);
    }

    if (args.size() == 1) {
        loadEsiTexts(args[0], texts);
    }

    MasterDevice m(getSingleMasterIndex());
    m.open(MasterDevice::Read);
    slaves = selectedSlaves(m);
    if (slaves.size() != 1) {
        throwSingleSlaveRequired(slaves.size());
    }
    slavePosition = slaves.front().position;

    try {
        maxMessages = readSubIndexU8(m, slavePosition, DIAG_SUB_MAX_MESSAGES);
        newest = readSubIndexU8(m, slavePosition, DIAG_SUB_NEWEST);
    } catch (MasterDeviceException &) {
        err << "Slave does not provide a diagnosis history (0x10F3).";
        throwCommandException(err);
    }

    if (!maxMessages) {
        cout << "Diagnosis history is empty." << endl;
        m.close();
        return;
    }

    /* The messages live in a ring buffer, so subindex order is only
     * chronological until it first wraps. Walk it starting at the slot after
     * the newest one, which is the oldest once wrapped and empty before that.
     * Timestamps cannot be used to order these: they are the slave's local
     * time since power-on and restart from zero across a power cycle. */
    unsigned int count = maxMessages;
    unsigned int start = 0;

    if (DIAG_SUB_FIRST_MESSAGE + count - 1 > 0xff) {
        count = 0x100 - DIAG_SUB_FIRST_MESSAGE;
    }
    if (newest >= DIAG_SUB_FIRST_MESSAGE
            && newest < DIAG_SUB_FIRST_MESSAGE + count) {
        start = newest - DIAG_SUB_FIRST_MESSAGE + 1;
    }

    for (unsigned int i = 0; i < count; i++) {
        unsigned int sub = DIAG_SUB_FIRST_MESSAGE + (start + i) % count;
        Message msg;

        if (!readMessage(m, slavePosition, (uint8_t) sub, msg)) {
            continue;
        }

        TextMap::const_iterator ti = texts.find(msg.textId);
        string params = formatParams(msg.params);

        cout << (sub == newest ? "*" : " ")
            << " [" << setw(6) << fixed << setprecision(2)
            << (double) msg.timestamp / 1e9 << "s]"
            << " " << setw(7) << left << typeName(msg.flags) << right
            << " code 0x" << hex << setfill('0') << setw(8) << msg.diagCode
            << " text 0x" << setw(4) << msg.textId << setfill(' ') << dec;

        if (ti != texts.end()) {
            cout << "  " << ti->second;
        }
        if (!params.empty()) {
            cout << "  (" << params << ")";
        }
        cout << endl;
        shown++;
    }

    if (!shown) {
        cout << "Diagnosis history is empty." << endl;
    } else {
        cout << "(* = newest message)" << endl;
    }

    m.close();
}

/****************************************************************************/
