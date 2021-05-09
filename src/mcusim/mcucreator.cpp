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

#include <QDomDocument>
#include <QFileInfo>
#include <QObject>

#include "mcucreator.h"
#include "e_mcu.h"
#include "mcu.h"
#include "mcuport.h"
#include "mcupin.h"

#include "usarttx.h"
#include "usartrx.h"

// Cores
#include "avrcore.h"
#include "avrtimer.h"
#include "avrocunit.h"
#include "avrinterrupt.h"
#include "avrusart.h"
#include "avradc.h"
#include "avrtwi.h"

#include "pic14core.h"

#include "i51core.h"
#include "i51timer.h"
#include "i51interrupt.h"
#include "i51usart.h"

#include "utils.h"


QString McuCreator::m_core = "";
QString McuCreator::m_CompName = "";
QString McuCreator::m_basePath = "";
Mcu*    McuCreator::m_mcuComp = NULL;
eMcu*   McuCreator::mcu = NULL;
QDomElement McuCreator::m_stackEl;

McuCreator::McuCreator(){}
McuCreator::~McuCreator(){}

int McuCreator::createMcu( Mcu* mcuComp, QString name )
{
    m_CompName = name;
    m_mcuComp = mcuComp;

    mcu = &(mcuComp->m_eMcu);
    QString dataFile = mcuComp->m_dataFile;
    m_basePath = QFileInfo( dataFile ).absolutePath();
    dataFile = QFileInfo( dataFile ).fileName();

    return processFile( dataFile );
}

int McuCreator::processFile( QString fileName )
{
    fileName = m_basePath+"/"+fileName;
    QDomDocument domDoc = fileToDomDoc( fileName, "SubCircuit::SubCircuit" );
    if( domDoc.isNull() ) { return 1; }

    QDomElement root = domDoc.documentElement();

    if( root.hasAttribute("core") )       m_core = root.attribute( "core" );
    if( root.hasAttribute("data") )       createDataMem( root.attribute( "data" ).toUInt(0,0) );
    if( root.hasAttribute("prog") )       createProgMem( root.attribute( "prog" ).toUInt(0,0) );
    //if( root.hasAttribute("eeprom") )     createEeprom( root.attribute( "eeprom" ).toUInt(0,0) );
    if( root.hasAttribute("progword") )   mcu->m_wordSize = root.attribute( "progword" ).toUInt(0,0);
    if( root.hasAttribute("inst_cycle") ) mcu->m_cPerInst = root.attribute( "inst_cycle" ).toDouble();

    QDomNode node = root.firstChild();
    while( !node.isNull() )
    {
        QDomElement el = node.toElement();

        if     ( el.tagName() == "regblock" )   createRegisters( &el );
        else if( el.tagName() == "datablock" )  createDataBlock( &el );
        else if( el.tagName() == "stack" )      m_stackEl = el;
        else if( el.tagName() == "status" )     createStatusReg( &el );
        else if( el.tagName() == "interrupts" ) createInterrupts( &el );
        else if( el.tagName() == "port" )       createPort( &el );
        else if( el.tagName() == "timer" )      createTimer( &el );
        else if( el.tagName() == "usart" )      createUsart( &el );
        else if( el.tagName() == "adc" )        createAdc( &el );
        else if( el.tagName() == "twi" )        createTwi( &el );
        else if( el.tagName() == "include" )    processFile( el.attribute("file") );

        node = node.nextSibling();
    }
    if( root.hasAttribute("core") ) createCore( m_core );

    return 0;
}

void McuCreator::createProgMem( uint32_t size )
{
    mcu->m_flashSize = size;
    mcu->m_progMem.resize( size, 0xFFFF );
}

void McuCreator::createDataMem( uint32_t size )
{
    mcu->m_ramSize = size;
    mcu->m_dataMem.resize( size, 0 );
    mcu->m_addrMap.resize( size, 0xFFFF ); // Not Maped values = 0xFFFF -> don't exist
}

void McuCreator::createDataBlock( QDomElement* d )
{
    uint16_t datStart = d->attribute("start").toUInt(0,0);
    uint16_t datEnd   = d->attribute("end").toUInt(0,0);
    uint16_t mapTo    = datStart;

    if( datEnd >= mcu->m_ramSize )
    {
        qDebug() << "McuCreator::createDataBlock  ERROR creating DataBlock";
        qDebug() << "dataMemSize  = " << mcu->m_ramSize;
        qDebug() << "dataBlockEnd = " << datEnd;
        return;
    }
    if( d->hasAttribute("mapto") ) mapTo = d->attribute("mapto").toUInt(0,0);

    for( int i=datStart; i<=datEnd; ++i )
    {
        mcu->m_addrMap[i] = mapTo;
        mapTo++;
    }
}

void McuCreator::createRegisters( QDomElement* e )
{
    uint16_t regStart = e->attribute("start").toUInt(0,0);
    uint16_t regEnd   = e->attribute("end").toUInt(0,0);
    uint16_t offset   = e->attribute("offset").toUInt(0,0);

    if( regEnd >= mcu->m_ramSize )
    {
        qDebug() << "McuCreator::createRegisters  ERROR creating Registers";
        qDebug() << "dataMemSize  = " << mcu->m_ramSize;
        qDebug() << "RegistersEnd = " << regEnd;
        return;
    }

    if( regStart < mcu->m_regStart ) mcu->m_regStart = regStart;
    if( regEnd   > mcu->m_regEnd )   mcu->m_regEnd   = regEnd;

    QDomNode node = e->firstChild();
    while( !node.isNull() ) // Create Registers
    {
        QDomElement el = node.toElement();

        if( el.tagName() == "register" )
        {
            QString  regName = el.attribute("name");
            uint16_t regAddr = el.attribute("addr").toUInt(0,0)+offset;
            uint8_t resetVal = el.attribute("reset").toUInt(0,0);
            //uint8_t writeMask = el.attribute("writemask").toUInt(0,0); // Write mask is inverted: 0 means write allowed

            mcu->m_addrMap[regAddr] = regAddr;

            eMcu::regInfo_t regInfo = { regAddr, resetVal/*, writeMask*/ };
            mcu->m_regInfo.insert( regName, regInfo );

            QString bits = el.attribute( "bits" );
            if( !bits.isEmpty() ) // Create bitMasks
            {
                if( bits == "0-7"){
                    for( int i=0; i<8; ++i )
                    {
                        QString bitName = regName+QString::number( i );
                        uint8_t mask = 1<<i;
                        mcu->m_bitMasks.insert( bitName, mask );
                        mcu->m_bitRegs.insert( bitName, regAddr );
                    }
                }else{
                    QStringList bitList = bits.split(",");
                    for( int i=0; i< bitList.size(); ++i )
                    {
                        QString bitName = bitList.at( i );
                        if( bitName == "0" ) continue;
                        uint8_t mask = 1<<i;
                        mcu->m_bitMasks.insert( bitName, mask );
                        mcu->m_bitRegs.insert( bitName, regAddr );
                    }
                }
            }
        }else if( el.tagName() == "alias" )
        {
            uint16_t regAddr = el.attribute("addr").toUInt(0,0)+offset;
            uint16_t mapTo   = el.attribute("mapto").toUInt(0,0);
            mcu->m_addrMap[regAddr] = mapTo;
        }
        node = node.nextSibling();
    }
}

void McuCreator::createStatusReg( QDomElement* s ) // CReate STATUS Reg
{
    QString sregName = s->attribute( "streg" );
    uint16_t addr = mcu->m_regInfo.value( sregName ).address;
    mcu->m_sregAddr = addr;
    mcu->m_sreg.resize( 8 );

    mcu->watchRegister( addr, R_WRITE, mcu, &eMcu::writeStatus );
    mcu->watchRegister( addr, R_READ,  mcu, &eMcu::readStatus );
}

void McuCreator::createInterrupts( QDomElement* i )
{
    QString enable = i->attribute("enable");
    mcu->watchBitNames( enable, R_WRITE, (eMcu*)mcu, &eMcu::enableInterrupts );

    QDomNode node = i->firstChild();
    while( !node.isNull() )
    {
        QDomElement el = node.toElement();

        QString  intName    = el.attribute("name");
        uint16_t intVector  = el.attribute("vector").toUInt(0,0);

        Interrupt* iv = NULL;
        if     ( m_core == "8051" ) iv = I51Interrupt::getInterrupt( intName, intVector, (eMcu*)mcu );
        else if( m_core == "AVR" )  iv = AVRInterrupt::getInterrupt( intName, intVector, (eMcu*)mcu );
        if( !iv ) return;

        mcu->m_interrupts.m_intList.insert( intName, iv );
        iv->m_interrupts = &(mcu->m_interrupts);

        enable = el.attribute("enable");
        mcu->watchBitNames( enable, R_WRITE, iv, &Interrupt::enableFlag );

        QString intFlag = el.attribute("flag");
        iv->m_flagMask = mcu->m_bitMasks.value( intFlag );
        iv->m_flagReg  = mcu->m_bitRegs.value( intFlag );

        QString intPrio = el.attribute("priority");
        bool ok = false;
        uint8_t prio = intPrio.toUInt(&ok,0);
        if( ok ) iv->setPriority( prio );
        else     mcu->watchBitNames( intPrio, R_WRITE, iv, &Interrupt::setPriority );

        if( el.hasAttribute("mode") )
        {
            QString mode = el.attribute("mode");
            mcu->watchBitNames( mode, R_WRITE, iv, &Interrupt::setMode );
        }
        node = node.nextSibling();
    }
}

void McuCreator::createPort( QDomElement* p )
{
    McuPort* port = new McuPort( mcu );
    port->m_name = p->attribute( "name" );
    mcu->m_ports.m_portList.insert( port->name(), port );

    // Create PORT Pins
    port->m_numPins = p->attribute( "pins" ).toUInt(0,0);
    port->m_pins.resize( port->m_numPins );
    for( int i=0; i<port->m_numPins; ++i )
    {
        port->m_pins[i] = new McuPin( port, i, port->m_name+QString::number(i), m_mcuComp );
    }
    uint16_t addr = 0;
    if( p->hasAttribute( "outreg" ) ) // Connect to PORT Out Register
    {
        addr = mcu->getRegAddress( p->attribute( "outreg" ) );
        port->m_outAddr = addr;
        port->m_outReg  = mcu->m_dataMem.data()+addr;
        mcu->watchRegister( addr, R_WRITE, port, &McuPort::outChanged );
    }
    if( p->hasAttribute( "inreg" ) ) // Connect to PORT In Register
    {
        addr = mcu->getRegAddress( p->attribute( "inreg" ) );
        port->m_inAddr = addr;
        port->m_inReg  = mcu->m_dataMem.data()+port->m_inAddr;
        mcu->watchRegister( addr, R_READ, port, &McuPort::readInReg );
    }
    if( p->hasAttribute( "dirreg" ) ) // Connect to PORT Dir Register
    {
        QString dirreg = p->attribute( "dirreg" );
        if( dirreg.startsWith("!") )
        {
            port->m_dirInv = true;
            dirreg.remove( 0, 1 );
        }
        addr = mcu->getRegAddress( dirreg );
        port->m_dirAddr = addr;
        port->m_dirReg  = mcu->m_dataMem.data()+addr;
        mcu->watchRegister( addr, R_WRITE, port, &McuPort::dirChanged );
    }
    if( p->hasAttribute("dirmask") ) // Permanent Directions
    {
        uint8_t dirMask = p->attribute("dirmask").toUInt( 0, 2 );
        for( int i=0; i<port->m_numPins; ++i )
            port->m_pins[i]->m_dirMask = (dirMask & 1<<i);
    }
    if( p->hasAttribute("pullups") ) // Permanent Pullups
    {
        uint8_t pullup = p->attribute("pullups").toUInt( 0, 2 );
        for( int i=0; i<port->m_numPins; ++i )
            port->m_pins[i]->m_puMask = (pullup & 1<<i);
    }
    if( p->hasAttribute("opencol") ) // OPen Drain
    {
        uint8_t opencol = p->attribute("opencol").toUInt( 0, 2 );
        for( int i=0; i<port->m_numPins; ++i )
            if( opencol & 1<<i ) port->m_pins[i]->m_openColl = true;
    }
    // Interrupts and...
    QDomNode node = p->firstChild();
    while( !node.isNull() )
    {
        QDomElement el = node.toElement();

        /// TODO: PORT interrupts
        /*if( el.tagName() == "raiseint" )  // Interrupts
        {
            QString intName = el.attribute("intname");
            Interrupt* inte = mcu->m_interrupts.m_intList.value( intName );

            QString source = el.attribute("source");
            mcu->watchBitNames( source, R_WRITE, inte, &Interrupt::raise );
        }*/
        node = node.nextSibling();
    }
}

void McuCreator::createTimer( QDomElement* t )
{
    McuTimer* timer = NULL;
    QString timerName = t->attribute("name");

    if     ( m_core == "8051" ) timer = new I51Timer( mcu, timerName );
    else if( m_core == "AVR" )  timer = AvrTimer::makeTimer( mcu, timerName );
    else return;

    mcu->m_timers.m_timerList.insert( timerName, timer );

    if( t->hasAttribute("counter") )
    {
        QString counter = t->attribute("counter");
        QString lowByte = counter;
        QString highByte ="";

        if( counter.contains(",") )
        {
            QStringList regs = counter.split(",");

            lowByte = regs.takeFirst();
            highByte = regs.takeFirst();
        }
        if( !lowByte.isEmpty() )
        {
            timer->m_countL = mcu->getReg( lowByte );
            mcu->watchRegNames( lowByte, R_WRITE, timer, &McuTimer::countWriteL );
            mcu->watchRegNames( lowByte, R_READ,  timer, &McuTimer::updtCount );
        }
        if( !highByte.isEmpty() )
        {
            timer->m_countH = mcu->getReg( highByte );
            mcu->watchRegNames( highByte, R_WRITE, timer, &McuTimer::countWriteH );
            mcu->watchRegNames( highByte, R_READ,  timer, &McuTimer::updtCount );
        }
    }
    if( t->hasAttribute("enable") )
    {
        QString enable = t->attribute("enable");
        mcu->watchBitNames( enable, R_WRITE, timer, &McuTimer::enable );
    }
    setConfigRegs( t, timer );

    QDomNode node = t->firstChild();
    while( !node.isNull() )
    {
        QDomElement el = node.toElement();

        if     ( el.tagName() == "raiseint" )  setInterrupt( &el, timer );
        else if( el.tagName() == "prescaler" )
        {
            QStringList prescalers = el.attribute("values").remove(" ").split(",");
            timer->m_prescList.resize( prescalers.size() );

            for( int i=0; i<prescalers.size(); ++i )
                timer->m_prescList[i] = prescalers.at(i).toUInt();
        }
        else if( el.tagName() == "extclock" )
        {
            /// TODO
        }
        else if( el.tagName() == "ocunit" )
        {
            McuOcUnit* ocUnit = NULL;
            if( m_core == "AVR" ) ocUnit = new AvrOcUnit( mcu, el.attribute("name") );
            if( !ocUnit ) continue;

            McuPin* pin = mcu->m_ports.getPin( el.attribute("pin") );
            QString ocrRegName = el.attribute("ocreg");
            //uint8_t*   ocrReg = mcu->getReg(  );

            timer->addocUnit( ocUnit );
            ocUnit->m_timer = timer;
            ocUnit->m_ocPin = pin;
            mcu->watchRegNames( ocrRegName, R_WRITE, ocUnit, &McuOcUnit::ocrChanged );
            //ocUnit->m_ocrReg = ocrReg;

            if( el.hasAttribute("configbits") )
            {
                QString configBits = el.attribute("configbits");
                mcu->watchBitNames( configBits, R_WRITE, ocUnit, &McuOcUnit::configure );
                ocUnit->m_configBits = mcu->getRegBits( configBits );
            }
        }
        node = node.nextSibling();
    }
}

void McuCreator::createUsart( QDomElement* u )
{
    QString name = u->attribute( "name" );
    McuUsart* usartM;
    if     ( m_core == "8051" ) usartM = new I51Usart( mcu, name );
    else if( m_core == "AVR" )  usartM = new AvrUsart( mcu, name );
    else return;

    McuUsart::m_usarts.insert( name, usartM );

    setConfigRegs( u, usartM );

    QDomNode node = u->firstChild();
    while( !node.isNull() )
    {
        QDomElement el = node.toElement();

        if( el.tagName() == "trunit" )
        {
            UartTR* trUnit;
            QString type = el.attribute( "type" );
            if     ( type == "tx" ) trUnit = usartM->m_sender;
            else if( type == "rx" ) trUnit = usartM->m_receiver;
            else continue;

            QString regName = el.attribute( "register" );

            if( type == "tx" )
                mcu->watchRegNames( regName, R_WRITE, trUnit, &UartTR::processData );
            else if( type == "rx" )
                usartM->m_rxRegister = mcu->getReg( regName );

            QString pinName = el.attribute( "pin" );
            trUnit->setPin( mcu->m_ports.getPin( pinName ) );

            if( el.hasAttribute("enable") )
            {
                QString enable = el.attribute( "enable" );
                mcu->watchBitNames( enable, R_WRITE, trUnit, &UartTR::enable );
            }
            if( el.hasAttribute("raiseint") )
            {
                QString intName = el.attribute("raiseint");
                Interrupt* inte = mcu->m_interrupts.m_intList.value( intName );
                trUnit->on_dataEnd.connect( inte, &Interrupt::raise );
            }
        }
        node = node.nextSibling();
    }
}

void McuCreator::createAdc( QDomElement* e )
{
    QString name = e->attribute( "name" );
    McuAdc* adc;
    if( m_core == "AVR" ) adc = new AvrAdc( mcu, name );
    else return;

    setConfigRegs( e, adc );

    if( e->hasAttribute("bits") )
    {
        bool ok = false;
        int bits = e->attribute("bits").toInt( &ok );
        if( ok ) adc->m_maxValue = pow( 2, bits )-1;
    }
    if( e->hasAttribute("valueregs") )
    {
        QString valueregs = e->attribute("valueregs");
        QString lowByte = valueregs;
        QString highByte ="";

        if( valueregs.contains(",") )
        {
            QStringList regs = valueregs.split(",");

            lowByte = regs.takeFirst();
            highByte = regs.takeFirst();
        }
        if( !lowByte.isEmpty() )  adc->m_ADCL = mcu->getReg( lowByte );
        if( !highByte.isEmpty() ) adc->m_ADCH = mcu->getReg( highByte );
    }
    if( e->hasAttribute("multiplex") )
    {
        QString configRegs = e->attribute("multiplex");
        mcu->watchRegNames( configRegs, R_WRITE, adc, &McuAdc::setChannel );
    }

    QDomNode node = e->firstChild();
    while( !node.isNull() )
    {
        QDomElement el = node.toElement();

        if     ( el.tagName() == "raiseint" ) setInterrupt( &el, adc );
        else if( el.tagName() == "prescaler" )
        {
            QStringList prescalers = el.attribute("values").remove(" ").split(",");
            adc->m_prescList.resize( prescalers.size() );

            for( int i=0; i<prescalers.size(); ++i )
                adc->m_prescList[i] = prescalers.at(i).toUInt();
        }
        else if( el.tagName() == "inputs" )
        {
            QString type = el.attribute("type");
            if( type == "PIN" )
            {
                QStringList pins = el.attribute("source").remove(" ").split(",");
                for( QString pinName : pins )
                {
                    McuPin* pin = mcu->m_ports.getPin( pinName );
                    if( pin ) adc->m_adcPin.emplace_back( pin );
                }
            }
        }
        node = node.nextSibling();
    }
}

void McuCreator::createTwi( QDomElement* e )
{
    QString name = e->attribute( "name" );
    McuTwi* twi;
    if( m_core == "AVR" ) twi = new AvrTwi( mcu, name );
    else return;

    if( e->hasAttribute("valueregs") )
    {
        QString twiReg = e->attribute("valueregs");
        if( !twiReg.isEmpty() )  twi->m_twiReg = mcu->getReg( twiReg );
    }
    if( e->hasAttribute("addressreg") )
    {
        QString addressReg = e->attribute("addressreg");
        if( !addressReg.isEmpty() ) twi->m_address = mcu->getReg( addressReg );
    }
    if( e->hasAttribute("twistatus") )
    {
        QString twiStatus = e->attribute("twistatus");
        if( !twiStatus.isEmpty() )
        {
            twi->m_twiStatus = mcu->getReg( twiStatus );
            mcu->watchRegNames( twiStatus, R_WRITE, twi, &McuTwi::writeStatus );
        }
    }
    setConfigRegs( e, twi );
}

void McuCreator::createCore( QString core )
{
    if     ( core == "AVR" )   mcu->cpu = new AvrCore( mcu );
    else if( core == "Pic14" ) mcu->cpu = new Pic14Core( mcu );
    else if( core == "8051" )  mcu->cpu = new I51Core( mcu );

    if( !m_stackEl.isNull() ) createStack( &m_stackEl );
}

void McuCreator::createStack( QDomElement* s )
{
    QString spReg = s->attribute("spreg");
    QStringList spRegs = spReg.split(",");

    spReg = spRegs.takeFirst();
    mcu->cpu->m_spl = mcu->getReg( spReg );
    if( !spRegs.isEmpty() )
    {
        spReg = spRegs.takeFirst();
        mcu->cpu->m_sph = mcu->getReg( spReg );
    }
    QString inc = s->attribute("increment");

    mcu->cpu->m_spPre = inc.contains("pre");
    mcu->cpu->m_spInc = ((inc.contains("inc"))?  1:-1);
}

void McuCreator::setInterrupt( QDomElement* el, McuModule* module )
{
    QString intName = el->attribute("intname");
    Interrupt* inte = mcu->m_interrupts.m_intList.value( intName );

    QString source  = el->attribute("source");
    if( source == "MAIN" ) module->interrupt.connect( inte, &Interrupt::raise );
}

void McuCreator::setConfigRegs( QDomElement* u, McuModule* module )
{
    if( u->hasAttribute("configregsA") )
    {
        QString regs = u->attribute("configregsA");
        mcu->watchRegNames( regs, R_WRITE, module, &McuModule::configureA );
    }
    if( u->hasAttribute("configregsB") )
    {
        QString regs = u->attribute("configregsB");
        mcu->watchRegNames( regs, R_WRITE, module, &McuModule::configureB );
    }
    if( u->hasAttribute("configbitsA") )
    {
        QString configBits = u->attribute("configbitsA");
        mcu->watchBitNames( configBits, R_WRITE, module, &McuModule::configureA );
        module->m_configBitsA = mcu->getRegBits( configBits );
    }
    if( u->hasAttribute("configbitsB") )
    {
        QString configBits = u->attribute("configbitsB");
        mcu->watchBitNames( configBits, R_WRITE, module, &McuModule::configureB );
        module->m_configBitsB = mcu->getRegBits( configBits );
    }
}
