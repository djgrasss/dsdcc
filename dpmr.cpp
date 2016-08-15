///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016 Edouard Griffiths, F4EXB.                                  //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <stdlib.h>
#include <iostream>
#include "dpmr.h"
#include "dsd_decoder.h"

namespace DSDcc
{

/*
 * DMR AMBE interleave schedule
 */
// bit 1
const int DSDdPMR::rW[36] = {
  0, 1, 0, 1, 0, 1,
  0, 1, 0, 1, 0, 1,
  0, 1, 0, 1, 0, 1,
  0, 1, 0, 1, 0, 2,
  0, 2, 0, 2, 0, 2,
  0, 2, 0, 2, 0, 2
};

const int DSDdPMR::rX[36] = {
  23, 10, 22, 9, 21, 8,
  20, 7, 19, 6, 18, 5,
  17, 4, 16, 3, 15, 2,
  14, 1, 13, 0, 12, 10,
  11, 9, 10, 8, 9, 7,
  8, 6, 7, 5, 6, 4
};

// bit 0
const int DSDdPMR::rY[36] = {
  0, 2, 0, 2, 0, 2,
  0, 2, 0, 3, 0, 3,
  1, 3, 1, 3, 1, 3,
  1, 3, 1, 3, 1, 3,
  1, 3, 1, 3, 1, 3,
  1, 3, 1, 3, 1, 3
};

const int DSDdPMR::rZ[36] = {
  5, 3, 4, 2, 3, 1,
  2, 0, 1, 13, 0, 12,
  22, 11, 21, 10, 20, 9,
  19, 8, 18, 7, 17, 6,
  16, 5, 15, 4, 14, 3,
  13, 2, 12, 1, 11, 0
};

const unsigned char DSDdPMR::m_fs2[12]      = {1, 1, 3, 3, 3, 3, 1, 3, 1, 3, 3, 1};
const unsigned char DSDdPMR::m_fs3[12]      = {1, 3, 3, 1, 3, 1, 3, 3, 3, 3, 1, 1};
const unsigned char DSDdPMR::m_preamble[12] = {1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3};

const unsigned char DSDdPMR::Hamming_12_8::m_H[12*4] = {
        1, 0, 1, 0, 1, 1, 0, 0,   1, 0, 0, 0,
        1, 1, 0, 1, 0, 1, 1, 0,   0, 1, 0, 0,
        1, 1, 1, 0, 1, 0, 1, 1,   0, 0, 1, 0,
        0, 1, 0, 1, 1, 0, 0, 1,   0, 0, 0, 1
//      0  1  2  3  4  5  6  7 <- correctable bit positions
};

void DSDdPMR::Hamming_12_8::init()
{
    // correctable bit positions given syndrome bits as index (see above)
    memset(m_corr, 0xFF, 16); // initialize with all invalid positions
    m_corr[0b1110] = 0;
    m_corr[0b0111] = 1;
    m_corr[0b1010] = 2;
    m_corr[0b0101] = 3;
    m_corr[0b1011] = 4;
    m_corr[0b1100] = 5;
    m_corr[0b0110] = 6;
    m_corr[0b0011] = 7;
}

DSDdPMR::DSDdPMR(DSDDecoder *dsdDecoder) :
        m_dsdDecoder(dsdDecoder),
        m_state(DPMRHeader),
        m_frameType(DPMRNoFrame),
        m_syncCycle(0),
        m_symbolIndex(0),
        m_frameIndex(-1),
        m_colourCode(0),
        m_calledId(0),
        m_ownId(0),
        w(0),
        x(0),
        y(0),
        z(0)
{
    initScrambling();
    initInterleaveIndexes();
}

DSDdPMR::~DSDdPMR()
{
}

void DSDdPMR::init()
{
    m_symbolIndex = 0;
    m_state = DPMRHeader;
}

void DSDdPMR::process() // just pass the frames for now
{
    switch(m_state)
    {
    case DPMRHeader:
        processHeader();
        break;
    case DPMRPostFrame:
        processPostFrame();
        break;
    case DPMRExtSearch:
        processExtSearch();
        break;
    case DPMRSuperFrame:
        processSuperFrame();
        break;
    case DPMREnd:
        processEndFrame();
        break;
    default:
        m_dsdDecoder->resetFrameSync(); // end
    };
}

void DSDdPMR::processHeader()
{
    int dibit = m_dsdDecoder->m_dsdSymbol.getDibit(); // get dibit from symbol

    if (m_symbolIndex == 0)
    {
        m_frameType = DPMRHeaderFrame;
        m_dsdDecoder->getLogger().log("DSDdPMR::processHeader: start\n"); // DEBUG
    }

    if (m_symbolIndex < 60) // HI0: TODO just pass for now
    {
        processHIn(m_symbolIndex, dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 60 + 12) // Accumulate colour code di-bits
    {
        processColourCode(m_symbolIndex - 60, dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 60 + 12 + 60) // HI1: TODO just pass for now
    {
        processHIn(m_symbolIndex - (60 + 12), dibit);
        m_symbolIndex++;

        if (m_symbolIndex == 60 + 12 + 60) // header complete
        {
            m_state = DPMRPostFrame;
            m_symbolIndex = 0;
            m_frameIndex = 0;
        }
    }
    else // out of sync => terminate
    {
    	m_frameType = DPMRNoFrame;
        m_dsdDecoder->resetFrameSync(); // end
    }
}

void DSDdPMR::processHIn(int symbolIndex, int dibit) // FIXME
{
    m_bitBufferRx[dI120[2*symbolIndex]]     = ((dibit >> 1) & 1) ^ m_scrambleBits[2*symbolIndex]; // MSB
    m_bitBufferRx[dI120[2*symbolIndex + 1]] = (dibit & 1) ^ m_scrambleBits[2*symbolIndex + 1];    // LSB

    if (symbolIndex == 59)
    {
        bool hammingStatus = m_hamming.decode(m_bitBufferRx, m_bitBuffer, 10);

        if (checkCRC8(m_bitBuffer, 72)) // CRC8 check OK
        {
            // collect data
            int ht     = (m_bitBuffer[0]<<3) + (m_bitBuffer[1]<<2) + (m_bitBuffer[2]<<1) + m_bitBuffer[3];
            int mode   = (m_bitBuffer[52]<<2) + (m_bitBuffer[53]<<1) + m_bitBuffer[54];
            int format = (m_bitBuffer[55]<<3) + (m_bitBuffer[56]<<2) + (m_bitBuffer[57]<<1) + m_bitBuffer[58];
            int calledId = 0, ownId = 0;

            for (int i = 0; i < 24; i++)
            {
                calledId += (m_bitBuffer[4+23-i]) << i;
                ownId    += (m_bitBuffer[28+23-i]) << i;
            }

            m_dsdDecoder->getLogger().log("DSDdPMR::processHIn: HT: %d CID: %06X OID: %06X M: %d F: %02d\n",
                    ht, calledId, ownId, mode, format); // DEBUG
//            std::cerr << "DSDdPMR::processHIn:"
//                    << " HT: " << ht
//                    << " CID: " << calledId
//                    << " OID: " << ownId
//                    << " M: " << mode
//                    << " F: " << format << std::endl;

            if (calledId) {
                m_calledId = calledId;
            }

            if (ownId) {
                m_ownId = ownId;
            }

            if (ht < 9) {
                m_headerType = (DPMRHeaderType) ht;
            } else {
                m_headerType = DPMRReservedHeader;
            }

            if (mode < 6) {
                m_commMode = (DPMRCommMode) mode;
            } else {
                m_commMode = DPMRReservedMode;
            }

            if (format < 2) {
                m_commFormat = (DPMRCommFormat) format;
            } else {
                m_commFormat = DPMRReservedFormat;
            }
        }
        else
        {
            m_dsdDecoder->getLogger().log("DSDdPMR::processHIn: invalid CRC8 - Hamming: %d\n", hammingStatus); // DEBUG
            //std::cerr << "DSDdPMR::processHIn: invalid CRC8 - Hamming: "  << hammingStatus << std::endl; // DEBUG
        }
    }
}

void DSDdPMR::processPostFrame()
{
    int dibit = m_dsdDecoder->m_dsdSymbol.getDibit(); // get di-bit from symbol

    if (m_symbolIndex == 0)
    {
        m_dsdDecoder->getLogger().log("DSDdPMR::processPostFrame: start\n"); // DEBUG
    }

    if (m_symbolIndex < 12) // look for a sync
    {
        if (dibit > 1) // negatives (-1 or -3) => store 3 which maps to -3
        {
            m_syncDoubleBuffer[m_symbolIndex] = 3;
        }
        else // positives (+1 or +3) => store 1 which maps to +3
        {
            m_syncDoubleBuffer[m_symbolIndex] = 1;
        }

        m_symbolIndex++;

        if (m_symbolIndex == 12) // sync complete
        {
            m_dsdDecoder->getLogger().log("DSDdPMR::processPostFrame\n"); // DEBUG

            if (memcmp((const void *) m_syncDoubleBuffer, (const void *) m_fs2, 12) == 0) // start of superframes
            {
                m_state = DPMRSuperFrame;
                m_symbolIndex = 0;
            }
            else if (memcmp((const void *) m_syncDoubleBuffer, (const void *) m_fs3, 12) == 0) // end frame
            {
                m_state = DPMREnd;
                m_symbolIndex = 0;
            }
            // not sure it is in ETSI standard but some repeaters insert complete re-synchronization sequences in the flow
            else if ((memcmp((const void *) &m_syncDoubleBuffer[0], (const void *) m_preamble, 8) == 0)
                    || (memcmp((const void *) &m_syncDoubleBuffer[1], (const void *) m_preamble, 8) == 0)
                    || (memcmp((const void *) &m_syncDoubleBuffer[2], (const void *) m_preamble, 8) == 0)
                    || (memcmp((const void *) &m_syncDoubleBuffer[3], (const void *) m_preamble, 8) == 0))
            {
                m_frameType = DPMRNoFrame;
                m_dsdDecoder->resetFrameSync(); // trigger a full resync
            }
            else // look for sync extensively
            {
                std::cerr << "DSDdPMR::processPostFrame: start extensive sync search" << std::endl;
                m_frameType = DPMRExtSearchFrame;
                m_state = DPMRExtSearch;
                m_symbolIndex = 0;
                m_syncCycle = 0;
            }
        }
    }
    else if (m_symbolIndex < 12 + 5*36) // length of a payload frame
    {
        m_symbolIndex++;
    }
    else
    {
        m_symbolIndex = 0; // back to FS2 or FS3 sync search
    }
}

void DSDdPMR::processExtSearch()
{
    int dibit = m_dsdDecoder->m_dsdSymbol.getDibit(); // get di-bit from symbol

    if (m_symbolIndex >= 12)
    {
    	m_symbolIndex = 0; // new cycle

    	// a complete frame is 16*12 bytes i.e. 16 times a sync interval
    	// hence syncCycle goes from 0 to 15 inclusive
    	if (m_syncCycle < 15)
    	{
    	    m_syncCycle++;
    	}
    	else
    	{
    	    m_syncCycle = 0;
    	}
    }

	// compare around expected spot
    if ((m_syncCycle < 1) || (m_syncCycle > 14))
    {
        if (memcmp((const void *) &m_syncDoubleBuffer[m_symbolIndex], (const void *) m_fs2, 12) == 0)
        {
            m_dsdDecoder->getLogger().log("DSDdPMR::processExtSearch: stop extensive sync search (sync found)\n"); // DEBUG
            m_state = DPMRSuperFrame;
            m_symbolIndex = 0;
            processSuperFrame();
            return;
        }
        // not sure it is in ETSI standard but some repeaters insert complete re-synchronization sequences in the flow
        else if (memcmp((const void *) &m_syncDoubleBuffer[m_symbolIndex], (const void *) m_preamble, 12) == 0)
        {
            m_frameType = DPMRNoFrame;
            m_dsdDecoder->resetFrameSync(); // trigger a full resync
            return;
        }
    }

	// store
	m_syncDoubleBuffer[m_symbolIndex] = (dibit > 1 ? 3 : 1);
	m_syncDoubleBuffer[m_symbolIndex + 12] = (dibit > 1 ? 3 : 1);

	m_symbolIndex++;
}

void DSDdPMR::processSuperFrame()
{
    int dibit = m_dsdDecoder->m_dsdSymbol.getDibit(); // get di-bit from symbol

    if (m_symbolIndex == 0) // new frame
    {
        m_frameType = DPMRPayloadFrame;
        m_dsdDecoder->getLogger().log("DSDdPMR::processSuperFrame: start\n"); // DEBUG
    }

    if (m_symbolIndex < 36) // Start of frame 0 - CCH0
    {
        processCCH(m_symbolIndex, dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144) // TCH0
    {
        processTCH(m_symbolIndex - 36, dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144 + 12) // Start of frame 1 - CC0
    {
        m_frameIndex++;
        processColourCode(m_symbolIndex - (36 + 144), dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144 + 12 + 36) // CCH1
    {
        processCCH(m_symbolIndex - (36 + 144 + 12), dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144 + 12 + 36 + 144) // TCH1
    {
        processTCH(m_symbolIndex - (36 + 144 + 12 + 36), dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144 + 12 + 36 + 144 + 12) // Start of frame 2 - FS2-1
    {
        m_frameIndex++;
        processFS2(m_symbolIndex - (36 + 144 + 12 + 36 + 144), dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144 + 12 + 36 + 144 + 12 + 36) // CCH2
    {
        processCCH(m_symbolIndex - (36 + 144 + 12 + 36 + 144 + 12), dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144 + 12 + 36 + 144 + 12 + 36 + 144) // TCH2
    {
        processTCH(m_symbolIndex - (36 + 144 + 12 + 36 + 144 + 12 + 36), dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144 + 12 + 36 + 144 + 12 + 36 + 144 + 12) // Start of frame 3 - CC1
    {
        m_frameIndex++;
        processColourCode(m_symbolIndex - (36 + 144 + 12 + 36 + 144 + 12 + 36 + 144), dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144 + 12 + 36 + 144 + 12 + 36 + 144 + 12 + 36) // CCH3
    {
        processCCH(m_symbolIndex - (36 + 144 + 12 + 36 + 144 + 12 + 36 + 144 + 12), dibit);
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 36 + 144 + 12 + 36 + 144 + 12 + 36 + 144 + 12 + 36 + 144) // TCH3
    {
        processTCH(m_symbolIndex - (36 + 144 + 12 + 36 + 144 + 12 + 36 + 144 + 12 + 36), dibit);
        m_symbolIndex++;

        if (m_symbolIndex == 36 + 144 + 12 + 36 + 144 + 12 + 36 + 144 + 12 + 36 + 144) // end of super frame
        {
            m_frameType = DPMRNoFrame; // look for continuation or end
            m_state = DPMRPostFrame;
            m_symbolIndex = 0;
            m_frameIndex = 0;
        }
    }
    else // shouldn´t go there => out of sync error
    {
    	m_frameType = DPMRNoFrame;
        m_dsdDecoder->resetFrameSync(); // end
    }
}

void DSDdPMR::processEndFrame()
{
    if (m_symbolIndex == 0)
    {
    	m_frameType = DPMREndFrame;
        m_dsdDecoder->getLogger().log("DSDdPMR::processEndFrame: start\n"); // DEBUG
    }

    if (m_symbolIndex < 18) // END0: TODO: just pass for now
    {
        m_symbolIndex++;
    }
    else if (m_symbolIndex < 18 + 18) // END1: TODO: just pass for now
    {
        m_symbolIndex++;
    }
    else // terminated
    {
    	m_frameType = DPMRNoFrame;
        m_dsdDecoder->resetFrameSync(); // end
    }
}

void DSDdPMR::processColourCode(int symbolIndex, int dibit)
{
    m_colourBuffer[symbolIndex] = (dibit > 1 ? 1 : 0); // 01->0, 11->1 with 00 and 01 on the same positive side and 10 and 11 on the same negative side

    if (symbolIndex == 11) // last symbol
    {
        m_colourCode = 0;

        for (int i = 11, n = 0; i >= 0; i--, n++) // colour code is stored MSB first
        {
            if (m_colourBuffer[i] == 1)
            {
                m_colourCode += (1<<n); // bit is 1
            }
        }

        m_dsdDecoder->getLogger().log("DSDdPMR::processColourCode: %d\n", m_colourCode); // DEBUG
    }
}

void DSDdPMR::processFS2(int symbolIndex, int dibit)
{
    if ((dibit == 0) || (dibit == 1)) // positives (+1 or +3) => store 1 which maps to +3
    {
        m_syncDoubleBuffer[symbolIndex] = 1;
    }
    else
    {
        m_syncDoubleBuffer[symbolIndex] = 3;
    }

    if (symbolIndex == 11) // last symbol
    {
        if (memcmp((const void *) m_syncDoubleBuffer, (const void *) m_fs2, 12) == 0) // start of superframes
        {
            // nothing
            m_frameType = DPMRPayloadFrame;
        }
        else if (memcmp((const void *) m_syncDoubleBuffer, (const void *) m_fs3, 12) == 0) // end frame
        {
            m_state = DPMREnd;
            m_symbolIndex = 0;
        }
        else
        {
            m_dsdDecoder->getLogger().log("DSDdPMR::processFS2: start extensive sync search\n"); // DEBUG
            m_frameType = DPMRExtSearchFrame;
            m_state = DPMRExtSearch;
            m_symbolIndex = 0;
            m_syncCycle = 0;
        }
    }
}

void DSDdPMR::processCCH(int symbolIndex, int dibit)
{
    m_bitBufferRx[dI72[2*symbolIndex]]     = ((dibit >> 1) & 1) ^ m_scrambleBits[2*symbolIndex]; // MSB
    m_bitBufferRx[dI72[2*symbolIndex + 1]] = (dibit & 1) ^ m_scrambleBits[2*symbolIndex + 1];    // LSB

    if (symbolIndex == 35)
    {
// FIXME
//        if (m_hamming.decode(m_bitBufferRx, m_bitBuffer, 6)) // Hamming decode successful
//        {
//            if (checkCRC7(m_bitBuffer, 41)) // CRC7 check OK
//            {
//                std::cerr << "DSDdPMR::processCCH: success" << std::endl;
//            }
//            else
//            {
//                std::cerr << "DSDdPMR::processCCH: invalid CRC7" << std::endl;
//            }
//        }
//        else
//        {
//            //std::cerr << "DSDdPMR::processCCH: Hamming(12,8) failed" << std::endl;
//        }

        m_frameType = DPMRVoiceframe; // TODO
    }
}

void DSDdPMR::processTCH(int symbolIndex, int dibit)
{
    if (m_frameType == DPMRVoiceframe)
    {
        processVoiceFrame(symbolIndex % 36, dibit);
    }
    else
    {
        // TODO: assume only voice for new
    }
}

void DSDdPMR::processVoiceFrame(int symbolIndex, int dibit)
{
    if ((symbolIndex == 0) && (m_dsdDecoder->m_opts.errorbars == 1))
    {
        m_dsdDecoder->getLogger().log("\nMBE: ");
    }

    if (symbolIndex % 36 == 0)
    {
        w = rW;
        x = rX;
        y = rY;
        z = rZ;
        memset((void *) m_dsdDecoder->m_mbeDVFrame, 0, 9); // initialize DVSI frame
    }

    m_dsdDecoder->ambe_fr[*w][*x] = (1 & (dibit >> 1)); // bit 1
    m_dsdDecoder->ambe_fr[*y][*z] = (1 & dibit);        // bit 0
    w++;
    x++;
    y++;
    z++;

    storeSymbolDV(symbolIndex % 36, dibit); // store dibit for DVSI hardware decoder

    if (symbolIndex % 36 == 35)
    {
        m_dsdDecoder->m_mbeDecoder.processFrame(0, m_dsdDecoder->ambe_fr, 0);
        m_dsdDecoder->m_mbeDVReady = true; // Indicate that a DVSI frame is available

        if (m_dsdDecoder->m_opts.errorbars == 1)
        {
            m_dsdDecoder->getLogger().log(".");
        }
    }
}

void DSDdPMR::storeSymbolDV(int dibitindex, unsigned char dibit, bool invertDibit)
{
    if (m_dsdDecoder->m_mbelibEnable)
    {
        return;
    }

    if (invertDibit)
    {
        dibit = DSDcc::DSDSymbol::invert_dibit(dibit);
    }

    m_dsdDecoder->m_mbeDVFrame[dibitindex/4] |= (dibit << (6 - 2*(dibitindex % 4)));
}

void DSDdPMR::initScrambling()
{
    unsigned char dibit;

    m_scramblingGenerator.init();

    for (int i = 0; i < 120; i++)
    {
        m_scrambleBits[i] = m_scramblingGenerator.next() & 1;
    }
}

void DSDdPMR::initInterleaveIndexes()
{
    for (int i = 0; i < 72; i++)
    {
        dI72[i] = 12 * (i % 6) + (i / 6);
    }

    for (int i = 0; i < 120; i++)
    {
        dI120[i] = 12 * (i % 10) + (i / 10);
    }
}

bool DSDdPMR::checkCRC7(unsigned char *bits, int nbBits)
{
    memcpy(m_bitWork, bits, nbBits);
    memset(&m_bitWork[nbBits], 0, 7);

    for (int i = 0; i < nbBits; i++)
    {
        if (m_bitWork[i] == 1) // divide by X⁷+X³+1 (10001001)
        {
            m_bitWork[i]    = 0; // X⁷
            m_bitWork[i+4] ^= 1; // X³
            m_bitWork[i+7] ^= 1; // 1
        }
    }

    if (memcmp(&bits[nbBits], &m_bitWork[nbBits], 7) == 0) // CRC OK
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool DSDdPMR::checkCRC8(unsigned char *bits, int nbBits)
{
    memcpy(m_bitWork, bits, nbBits);
    memset(&m_bitWork[nbBits], 0, 8);

    for (int i = 0; i < nbBits; i++)
    {
        if (m_bitWork[i] == 1) // divide by X⁸+X²+X+1  (100000111)
        {
            m_bitWork[i]    = 0; // X⁸
            m_bitWork[i+6] ^= 1; // X²
            m_bitWork[i+7] ^= 1; // X
            m_bitWork[i+8] ^= 1; // 1
        }
    }

    if (memcmp(&bits[nbBits], &m_bitWork[nbBits], 8) == 0) // CRC OK
    {
        return true;
    }
    else
    {
        return false;
    }
}

DSDdPMR::LFSRGenerator::LFSRGenerator()
{
    init();
}

DSDdPMR::LFSRGenerator::~LFSRGenerator()
{
}

void DSDdPMR::LFSRGenerator::init()
{
    m_sr = 0x3FF; // all ones
}

unsigned int DSDdPMR::LFSRGenerator::next()
{
    m_sr >>= 1;

    unsigned int res = m_sr & 1;
    unsigned int feedback = ((((m_sr >> 4) & 1) ^ res) << 9);

    m_sr = (m_sr & 0x1FF) | feedback; // insert feedback bit

    return res;
}

DSDdPMR::Hamming_12_8::Hamming_12_8()
{
    init();
}

DSDdPMR::Hamming_12_8::~Hamming_12_8()
{
}

bool DSDdPMR::Hamming_12_8::decode(unsigned char *rxBits, unsigned char *decodedBits, int nbCodewords)
{
    bool correctable = true;

    for (int ic = 0; ic < nbCodewords; ic++)
    {
        // calculate syndrome

        bool error = false;
        int syndromeI = 0; // syndrome index

        for (int is = 0; is < 4; is++)
        {
            syndromeI += (((rxBits[12*ic +  0] * m_H[12*is +  0])
                         + (rxBits[12*ic +  1] * m_H[12*is +  1])
                         + (rxBits[12*ic +  2] * m_H[12*is +  2])
                         + (rxBits[12*ic +  3] * m_H[12*is +  3])
                         + (rxBits[12*ic +  4] * m_H[12*is +  4])
                         + (rxBits[12*ic +  5] * m_H[12*is +  5])
                         + (rxBits[12*ic +  6] * m_H[12*is +  6])
                         + (rxBits[12*ic +  7] * m_H[12*is +  7])
                         + (rxBits[12*ic +  8] * m_H[12*is +  8])
                         + (rxBits[12*ic +  9] * m_H[12*is +  9])
                         + (rxBits[12*ic + 10] * m_H[12*is + 10])
                         + (rxBits[12*ic + 11] * m_H[12*is + 11])) % 2) << is;
        }

        // correct bit

        if (syndromeI > 0) // single bit error correction
        {
            if (m_corr[syndromeI] == 0xFF) // uncorrectable error
            {
                correctable = false;
            }
            else
            {
                rxBits[m_corr[syndromeI]] ^= 1; // flip bit
            }
        }

        // move information bits
        memcpy(&decodedBits[8*ic], &rxBits[12*ic], 8);
    }

    return correctable;
}

} // namespace DSDcc
