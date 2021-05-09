/***************************************************************************
 *   Copyright (C) 2020 by santiago González                               *
 *   santigoro@gmail.com                                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>.  *
 *                                                                         *
 ***************************************************************************/

#include "i51usart.h"
#include "usarttx.h"
#include "usartrx.h"
#include "mcutimer.h"
#include "e_mcu.h"

#define SCON *m_scon

I51Usart::I51Usart( eMcu* mcu,  QString name )
        : McuUsart( mcu, name )
{
    m_stopBits = 1;
    m_dataMask = 0xFF;
    m_parity   = parNONE;

    m_timerConnected = false;
    m_timer1 = mcu->getTimer( "TIMER1" );

    m_scon = mcu->getReg( "SCON" );
    m_bit9Tx = mcu->getRegBits( "TB8" );
    m_bit9Rx = mcu->getRegBits( "RB8" );
}
I51Usart::~I51Usart(){}

void I51Usart::configureA( uint8_t val ) //SCON
{
    uint8_t mode = val >> 6;
    if( mode == m_mode ) return;
    m_mode = mode;

    m_sender->enable( true );

    m_useTimer = false;

    switch( mode )
    {
        case 0:             // Synchronous
            /// TODO //setPeriod(  m_mcu->simCycPI() );// Fixed baudrate 32 or 64
            m_dataBits = 8;
            break;
        case 1:             // Asynchronous Timer1
            m_useTimer = true;
            m_dataBits = 8;
            break;
        case 2:             // Asynchronous MCU Clock
            setPeriod(  m_mcu->simCycPI() );// Fixed baudrate 32 or 64
            m_dataBits = 9;
            break;
        case 3:             // Asynchronous Timer1
            m_useTimer = true;
            m_dataBits = 9;
            break;
    }

    if( m_useTimer )
    {
        if( !m_timerConnected )
        {
            m_timerConnected = true;
            m_timer1->on_tov.connect( this, &I51Usart::step );
        }
        setPeriod( 0 );
    }
}

void I51Usart::step( uint8_t )
{
    if( !m_useTimer ) return;

    m_sender->runEvent();
    m_receiver->runEvent();
}

uint8_t I51Usart::getBit9()
{
    return getRegBitsVal( SCON, m_bit9Tx );
}

void I51Usart::setBit9( uint8_t bit )
{
    SCON &= ~m_bit9Rx.mask;
    if( bit ) SCON |= m_bit9Rx.mask;
}

