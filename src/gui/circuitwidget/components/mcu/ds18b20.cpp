/***************************************************************************
 *   Copyright( C) 2018 by santiago González                               *
 *   santigoro@gmail.com                                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *  ( at your option) any later version.                                   *
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

#include <QDebug>

#include <QPainter>
#include <QPushButton>
#include <QGraphicsProxyWidget>

#include "ds18b20.h"
#include "iopin.h"
#include "simulator.h"
#include "circuit.h"
#include "itemlibrary.h"

#include "utils.h"

#include "doubleprop.h"

Component* Ds18b20::construct( QObject* parent, QString type, QString id )
{ return new Ds18b20( parent, type, id ); }

LibraryItem* Ds18b20::libraryItem()
{
    return new LibraryItem(
        "DS18B20",
        tr( "Sensors" ),
        "ic_comp.png",
        "DS18B20",
        Ds18b20::construct );
}

QString QDeb_HEX( uint8_t* data, uint len )
{
    QString hexValue;
    for( uint i=0; i<len; i++) hexValue.append( val2hex( data[i] ) ).append(" ");
    return hexValue;
}

QString QDeb_HEX( std::vector<uint8_t> data )
{
    QString hexValue;
    for( uint i=0; i<data.size(); i++) hexValue.append( val2hex( data[i] ) ).append(" ");
    return hexValue;
}

Ds18b20::Ds18b20( QObject* parent, QString type, QString id )
       : Component( parent, type, id )
       , eElement( id )
{    
    m_area = QRect(-28,-16, 70, 32 );

    m_state = W1_IDLE;
    m_parPower = false;
    m_resolution = 12; // Should check what is default 9-12, should be writable in EEPROM
    m_tempInc = 0.5;
    setTemp( 22 );

    // This actually should be generated and saved with sim file
    // once component is dropped. It is unique ID to keep
    generateROM( 0x28 );

    m_TH_reg  = 0x4B; /// TODO, check default value
    m_TL_reg  = 0x46; /// TODO, check default value
    m_CFG_reg = 0x7F; /// TODO, check default value

    m_pin.resize(1);
    m_pin[0] = m_inpin = new IoPin( 180, QPoint(-36, 8), id+"-inPin", 0, this, openCo );

    m_inpin->setOutHighV( 5 );
    m_inpin->setLabelColor( QColor( 250, 250, 200 ) );
    m_inpin->setLabelText("DQ");

    QPushButton* u_button = new QPushButton();
    u_button->setMaximumSize( 9, 9 );
    u_button->setGeometry(-5,-5, 9, 9);
    u_button->setCheckable( false );
    u_button->setIcon(QIcon(":/su.png"));

    QGraphicsProxyWidget* proxy = Circuit::self()->addWidget( u_button );
    proxy->setParentItem( this );
    proxy->setPos( QPoint( -7, 4 ) );

    QPushButton* d_button = new QPushButton();
    d_button->setMaximumSize( 9, 9 );
    d_button->setGeometry(-5,-5, 9, 9);
    d_button->setCheckable( false );
    d_button->setIcon(QIcon(":/giu.png"));

    proxy = Circuit::self()->addWidget( d_button );
    proxy->setParentItem( this );
    proxy->setPos( QPoint( 9, 4 ) );

    connect( u_button, SIGNAL( pressed()),
             this,     SLOT (  upbuttonclicked()) );

    connect( d_button, SIGNAL( pressed()),
             this,     SLOT (  downbuttonclicked()) );

    m_font.setFamily("Ubuntu");
    m_font.setBold( true );
    setLabelPos(-24,-28 );

    Simulator::self()->addToUpdateList( this );

    addPropGroup( { tr("Main"), {
new DoubProp<Ds18b20>( "Temp"   , tr("Temperature")    ,"°C", this, &Ds18b20::temp   , &Ds18b20::setTemp ),
new DoubProp<Ds18b20>( "TempInc", tr("Temp. increment"),"°C", this, &Ds18b20::tempInc, &Ds18b20::setTempInc ),
    }} );
}
Ds18b20::~Ds18b20(){}

void Ds18b20::stamp()   // Called at Simulation Start
{
    m_lastTime = 0;
    m_rxReg = 0;
    m_lastBit = 7;
    m_write = false;
    m_lastIn = false;
    m_inpin->setPinMode( openCo );
    m_inpin->setOutState( true );
    m_inpin->changeCallBack( this, true );
}

void Ds18b20::updateStep()
{
    if( !m_changed ) return;
    m_changed = false;
    setTemp( m_temp );
    update();
}

void Ds18b20::voltChanged()                              // Called when Input Pin changes
{
    bool inState = m_inpin->getVolt() > 2.5;
    if( m_lastIn && !inState ) m_lastTime = Simulator::self()->circTime(); // Falling edge
    else if( !m_lastIn && inState )                                        // Rising edge
    {
        uint64_t time = Simulator::self()->circTime()-m_lastTime; // in picoseconds

        if( time > 475*1e6 )             // > 480 us : Reset
        {
            qDebug() <<"\n"<< idLabel() << "Ds18b20::voltChanged -------------- RESET"<<time/1e3<<"ns";
            m_rxReg = 0;
            m_bitIndex = 0;
            m_state = W1_COMMAND;
            m_write = false;
            pulse( 30, 80 ); // Send 80 us ( 60 to 240 us) pulse after 30 us ( 15 to 60 us)
        }
        else if( m_state > W1_IDLE )    // Active
        {
            if( time > 60*1e6 )         // > 60 us : 0 received
            {
                if( m_write ) qDebug()<< idLabel() << "ERROR: Master is reading data, Read pulse should be < 15 us";
                else          readBit( 0 );
            }
            else    ///// if( time < 15*1e6 )     // < 15 us : 1 received Or Read pulse
            {
                if( m_write ) writeBit();
                else          readBit( 1 );
            }
        }
    }
    m_lastIn = inState;
}

void Ds18b20::runEvent()
{
    if( m_pullDown )   // Pull down
    {
        m_pullDown = false;
        m_inpin->sheduleState( false, 0 );
        Simulator::self()->addEvent( m_pulse, this );
    }
    else              // Release
    {
        m_inpin->sheduleState( true, 0 );
        m_inpin->changeCallBack( this, true ); // Receive voltChange() CallBacks again
    }
}

void Ds18b20::sendData( uint8_t data, int size )
{
    m_txReg = data;
    m_bitIndex = 0;
    m_lastBit = size-1;
    m_write = true;
}

void Ds18b20::writeBit()
{
    if( (m_txReg & 1<<m_bitIndex) == 0 )  pulse( 1, 50 ); // Send a 50 us pulse after 1 us

    if( m_bitIndex == m_lastBit ) // Byte sent
    {
        m_bitIndex = 0;
        m_write = false;
        dataSent();
    }
    else m_bitIndex += 1;
}

void Ds18b20::dataSent() // Last data has been sent
{
    if( m_state != W1_SEARCH ) qDebug() << idLabel() << "Ds18b20::dataSent"<< val2hex( m_txReg );

    if( m_state == W1_SEARCH ) m_lastBit = 0; // Read 1 bit
    else if( !m_txBuff.empty() )   // Send next byte in Tx Buffer
    {
        m_txBuff.pop_back(); // Remove last sent byte
        if( !m_txBuff.empty() ) sendData( m_txBuff.back() ); // Send last byte in list, if list is not empty
    }
}

void Ds18b20::readBit( uint8_t bit )
{
    if( m_state == W1_MATCH )  // Compare bit received with ROM
    {
        if( (bit>0) != bitROM( m_bitIndex ) )
        {
            m_state = W1_IDLE; // No ROM match, we are out
            qDebug() <<idLabel()<<"Ds18b20::readBit       NO ROM match";
        }
    }
    else if( bit ) m_rxReg |= 1<<m_bitIndex;

    if( m_bitIndex == m_lastBit )   // Complete byte received
    {
        m_bitIndex = 0;
        m_lastBit = 7;   // Return to "byte" mode
        dataReceived();
        m_rxReg = 0;
    }
    else m_bitIndex += 1;
}

void Ds18b20::dataReceived() // Complete data has been received (it's in m_rxReg)
{
    if( m_state != W1_MATCH && m_state != W1_SEARCH ) qDebug() << idLabel()<< "Ds18b20::dataReceived" << val2hex( m_rxReg );

    switch( m_state )
    {
        case W1_IDLE: break;                      // Error, this shoild not happen
        case W1_COMMAND: command( m_rxReg ); break; // Command received
        case W1_DATA: break;
        case W1_MATCH:                            // ROM Match: we are online, wait for commands
        {
            m_state = W1_COMMAND;
            qDebug() <<idLabel()<<"Ds18b20::dataReceived     ROM match";
        }break;
        case W1_SEARCH:
        {
            m_bitSearch++;
            if( m_bitSearch == 64 )    // We passed Search ROM: wait for commands
            {
                m_state = W1_IDLE;
                m_lastBit = 7; // Return to normal byte reception
                qDebug() <<idLabel()<< "Ds18b20::dataReceived  :  Search ROM OK ";
            }
            else if((m_rxReg > 0) == m_bitROM ) sendSearchBit();   // Bit Match,  keep sending
            else{
                m_state = W1_IDLE; // We are out, Wait for next Reset signal
                qDebug() <<idLabel()<< "Ds18b20::dataReceived  :  Search ROM OUT";
            }
        }
    }
}

void Ds18b20::pulse( uint64_t time, uint64_t witdth ) // Time in us
{
    m_pullDown = true;
    m_pulse = witdth*1e6;                          // Keep line low for width us
    m_inpin->changeCallBack( this, false );        // Stop receiving voltChange() CallBacks
    Simulator::self()->addEvent( time*1e6, this ); // Send pulse after time us
}

// COMMANDS --------------------------------------------------

void Ds18b20::command( uint8_t cmd )
{
    m_state = W1_DATA;
    switch( cmd )
    {
        case 0x33: readROM();         break;
        case 0x44: convertTemp();     break;
        //case 0x48: qDebug() <<idLabel()<< "Ds18b20::command : Warning:  COPY SCRATCHPAD Not implemented"; break;
        //case 0x4E: qDebug() <<idLabel()<< "Ds18b20::command : Warning:  WRITE SCRATCHPAD Not implemented"; break;
        case 0x55: matchROM();        break;
        case 0xB4: readPowerSupply(); break;
        case 0xBE: readScratchpad();  break;
        case 0xCC: skipROM();         break;
        case 0xF0: searchROM();       break;
        default:
        {
            m_state = W1_IDLE;
            qDebug()<<idLabel() << "Ds18b20::command : Warning:  command Not implemented";
        }
    }
    m_lastCommand = cmd;
}

void Ds18b20::readROM() // Code: 33h : send ROM to Master
{
    qDebug() << idLabel() <<"Ds18b20::readROM"<< QDeb_HEX( m_ROM, 8 );

    m_txBuff.clear();
    for( int i=7; i>=0; i-- ) m_txBuff.push_back( m_ROM[i] );

    qDebug() << idLabel()<< "Tx Buffer:" << QDeb_HEX( m_txBuff );

    sendData( m_txBuff.back() );
}

void Ds18b20::convertTemp() // Code 44h, temperature already in the Scratchpad, nothing to do
{
    qDebug() <<idLabel()<< "Ds18b20::convertTemp";
    m_state = W1_IDLE;
    if( !m_parPower ) pulse( 1, 749 ); // Send a 749 us pulse after 1 us
}

void Ds18b20::matchROM() // Code 55h : read 64 bits and compare with ROM
{
    qDebug() <<idLabel()<< "Ds18b20::matchROM";
    m_lastBit = 63;
    m_state = W1_MATCH;
}

void Ds18b20::readPowerSupply() // Code B4h : using parasite power? pull down time???
{
    qDebug() <<idLabel()<< "Ds18b20::readPowerSupply"; // By now we don't use parasite power
    /// TODO add property
}

void Ds18b20::readScratchpad() // Code BEh send Scratchpad LSB
{
    m_txBuff.clear();
    for( int i=8; i>=0; i-- ) m_txBuff.push_back( m_scratchpad[i] );

    qDebug() << idLabel() <<"Ds18b20::readScratchpad :"<<QDeb_HEX( m_txBuff );

    sendData( m_txBuff.back() );
}

void Ds18b20::skipROM() // Code: CCh : This device is selected, Wait command
{
    qDebug() <<idLabel()<< "Ds18b20::skipROM";
    m_state = W1_COMMAND;
}

void Ds18b20::searchROM() // Code F0h
{
    qDebug() <<idLabel()<< "Ds18b20::searchROM";
    m_state = W1_SEARCH;
    m_bitSearch = 0;
    sendSearchBit();
}

void Ds18b20::sendSearchBit()
{
    m_bitROM = bitROM( m_bitSearch );
    uint8_t data = m_bitROM ? 1 : 2;
    sendData( data, 2 );
}

bool Ds18b20::bitROM( uint bitIndex )
{
    uint byte = bitIndex/8;
    uint bit  = bitIndex%8;
    return ( m_ROM[ byte ] & 1<<bit ) > 0;
}

// End Commands --------------------------------------------------------------------

void Ds18b20::setTemp( double t ) // This should convert temp wrote in UI
{
    m_temp = t;
    uint16_t temp = abs( m_temp) * 16.0; // Make it positive and shift to make integer value
    if(  m_temp < 0 ) temp = ~temp + 1;  // Temp under 0? Make 2nd complement

    //-10.125    °C 1111 1111 0101 1110 FF5Eh
    //-25.0625   °C 1111 1110 0110 1111 FF6Fh

    /// TODO alarm settings
    //m_TH_reg =
    //m_TL_reg =

    // store temperature to scratchpad
    m_scratchpad[0] = temp;
    m_scratchpad[1] = temp >> 8;
    m_scratchpad[2] = m_TH_reg;
    m_scratchpad[3] = m_TL_reg;
    m_scratchpad[4] = m_CFG_reg;
    m_scratchpad[5] = 0xFF; // Reserved. Default?
    m_scratchpad[6] = 0x0C; // Reserved. Default?
    m_scratchpad[7] = 0x10; // Reserved. Default?
    m_scratchpad[8] = crc8( m_scratchpad, 8 );

    qDebug() << idLabel() <<"Ds18b20::setTemp"<<QDeb_HEX( m_scratchpad, 9 );

    update();
}

uint8_t Ds18b20::crc8( uint8_t* addr, uint8_t len ) // DS18B20 crc8 calc
{
    uint8_t crc = 0;

    while( len--)
    {
        uint8_t inbyte = *addr++;
        for( uint8_t i=8; i; i-- )
        {
            uint8_t mix = (crc ^ inbyte) & 0x01;
            crc >>= 1;
            if( mix) crc ^= 0x8C;
            inbyte >>= 1;
        }
    }
    return crc;
}

void Ds18b20::generateROM( uint8_t familyCode ) // Generate unique ROM address
{
  // 8-bit CRC | 48 bit S/N | 8-BIT FAMILY CODE
  // MSB - LSB
  // FAMILY CODE = 28h for DS18B20

  m_ROM[0] = familyCode;
  m_ROM[1] = rand();
  m_ROM[2] = rand();
  m_ROM[3] = rand();
  m_ROM[4] = rand();
  m_ROM[5] = 0x00;
  m_ROM[6] = 0x00;

  /*// For test : 28 CA CB E2 03 00 00 B6
  m_ROM[0] = 0x28;
  m_ROM[1] = 0xCA;
  m_ROM[2] = 0xCB;
  m_ROM[3] = 0xE2;
  m_ROM[4] = 0x03;
  m_ROM[5] = 0x00;
  m_ROM[6] = 0x00; */

  m_ROM[7] = crc8( m_ROM, 7 );
  qDebug() << idLabel() <<"Ds18b20::generateROM"<<QDeb_HEX( m_ROM, 8 );
}

void Ds18b20::upbuttonclicked()
{
    m_temp += m_tempInc;
    if( m_temp > 125 ) m_temp = 125;
    m_changed = true;
}

void Ds18b20::downbuttonclicked()
{
    m_temp = m_temp - m_tempInc;
    if( m_temp < -55 ) m_temp = -55;
    m_changed = true;
}

void Ds18b20::paint( QPainter* p, const QStyleOptionGraphicsItem* option, QWidget* widget )
{
    Component::paint( p, option, widget );
    QPen pen( Qt::black, 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin );
    p->setPen( pen );
    p->setBrush(QColor( 50, 50, 70 ));
    p->drawRoundedRect( m_area, 2, 2 );

    p->setBrush( QColor(200, 220, 180) );
    p->drawRoundedRect( QRect(-24,-14, 60, 15 ),2,2 );

    m_font.setPixelSize(9);
    p->setFont( m_font );
    p->setPen( QColor(0, 0, 0) );    
    p->drawText( -22, -3, "C° "+QString::number( m_temp ) );
}

//#include "moc_ds18b20.cpp"
