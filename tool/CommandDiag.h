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

#ifndef __COMMANDDIAG_H__
#define __COMMANDDIAG_H__

#include <map>

#include "Command.h"

class MasterDevice;

/****************************************************************************/

/** Reads the ETG.1020 diagnosis history (object 0x10F3) of a slave.
 */
class CommandDiag:
    public Command
{
    public:
        CommandDiag();

        string helpString(const string &) const;
        void execute(const StringVector &);

    protected:
        /** One decoded diagnosis message. */
        struct Message {
            uint32_t diagCode;
            uint16_t flags;
            uint16_t textId;
            uint64_t timestamp;
            string params;
        };

        /** Text ID to message text, loaded from an ESI file. */
        typedef map<uint16_t, string> TextMap;

        enum {MaxMessageSize = 256};

        static bool readMessage(MasterDevice &, uint16_t slavePosition,
                uint8_t subIndex, Message &);
        static unsigned int readSubIndexU8(MasterDevice &,
                uint16_t slavePosition, uint8_t subIndex);
        void loadEsiTexts(const string &path, TextMap &) const;
        static string formatParams(const string &);
        static const char *typeName(uint16_t flags);
};

/****************************************************************************/

#endif
