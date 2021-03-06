/** Name: RfConfig.cpp
*
* Copyright(c) 2013 Loctronix Corporation
* http://www.loctronix.com
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#include "RfConfig.h"
#include <A2300/A2300_Defs.h>
#include <A2300/A2300InterfaceDefs.h>
#include <boost/math/special_functions/round.hpp>
#include <boost/math/special_functions/sign.hpp>
#include <boost/format.hpp>
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/assign/list_of.hpp>
//#include <boost/filesystem.hpp>
#include <uhd/exception.hpp>
#include <uhd/utils/msg.hpp>

#include "DciProperty.h"

using namespace uhd;

// Look-up table to map bandwidth (MHz) to A2300 entry.
static const double s_rfbandwidths[] =
	{ 1.5, 1.75, 2.5, 2.75, 3.0, 3.84, 5.0, 5.5, 6.0, 7.0, 8.75, 10.0, 12.0, 14.0, 20.0, 28.0};
static const int CT_BANDWIDTHS = (int)(sizeof(s_rfbandwidths)/sizeof(double));

RfConfig::RfConfig()
:
	 m_idRf(-1),
	 m_idComponent(0)
{
	// TODO Auto-generated constructor stub
}

RfConfig::~RfConfig()
{
	// TODO Auto-generated destructor stub
}

void RfConfig::Initialize(int idRf, byte idComponent, bool bTx, const uhd::fs_path pathroot, A2300_iface::sptr dci_ctrl, uhd::property_tree::sptr tree)
{
	//Save the relevant information.
	fs_path pathRf = pathroot / str(boost::format("%u") % idRf);
	m_idRf 			= idRf;
	m_bIsTx			= bTx;
	m_idComponent 	= idComponent;
	m_dci_ctrl  	= dci_ctrl;

	const std::string sName = ((m_bIsTx) ? "tx": "rx") + str(boost::format("%u") % idRf);
    tree->create<std::string>(pathRf / "name").set(sName);

    //RF Static connection information.
    tree->create<std::string>(pathRf / "connection").set("IQ");
    tree->create<bool>(pathRf / "enabled").set(true);
    tree->create<bool>(pathRf / "use_lo_offset").set(false);

    //Configure the RF Programmable Gain for this channel.
    tree->create<double>(pathRf / "gains/PGA/value")
        .subscribe(boost::bind(&RfConfig::SetPgaGain, this, _1))
        .set(A2300_TXGAIN_STEP*((double)((int)((((A2300_TXGAIN_MAX + A2300_TXGAIN_MIN)/2.0) + A2300_TXGAIN_STEP)/A2300_TXGAIN_STEP))));
    tree->create<meta_range_t>(pathRf / "gains/PGA/range")
        .publish(boost::bind( &RfConfig::GetPgaGainRange, this));

    //Configure Bandwidth value and range properties.
    tree->create<double>(pathRf / "bandwidth" / "value")
		.subscribe( boost::bind( &RfConfig::SetRfBandwidth, this, _1))
        .set(s_rfbandwidths[0]);
    tree->create<meta_range_t>(pathRf / "bandwidth" / "range")
        .publish(boost::bind(&RfConfig::GetRfBandwidthRange, this));

    //Configure RF Center Frequency value and range properties.
    tree->create<double>(pathRf / "freq" / "value")
    	.subscribe(boost::bind(&RfConfig::SetRfFrequency, this,  _1))
    	.set(2400e6);
    tree->create<meta_range_t>(pathRf / "freq" / "range")
        .publish(boost::bind(&RfConfig::GetRfFrequencyRange, this));

    //TODO Configure RF Sensors
    tree->create<int>(pathRf / "sensors");

    //Configure RF Path profiles as antenna options.  There are four different possibilities here.
    std::vector<std::string> ants;
    //Select wideband as the default antenna, since it is common to all RF chains (both Tx and Rx)
    std::string sDefault = "Wideband";
    if( idComponent == WCACOMP_RF0)
    {
    	if( m_bIsTx )
    	{
    		ants = boost::assign::list_of("Disabled")("Wideband");
    	}
    	else
    	{
    		ants = boost::assign::list_of("Disabled")("L1GpsInt")("L1GpsExt")("PcsExt")("Wideband");
    	}
    }
    else //Assume its WCACOMP_RF1
    {
    	if( m_bIsTx )
    	{
    		ants = boost::assign::list_of("Disabled")("IsmInt")("IsmExt")("Wideband");
    	}
    	else
    	{
    		ants = boost::assign::list_of("Disabled")("UhfExt")("IsmInt")("IsmExt")("Wideband");
    	}

    }
    tree->create<std::vector<std::string> >(pathRf / "antenna" / "options").set(ants);
    tree->create<std::string>(pathRf / "antenna" / "value")
        .subscribe(boost::bind(&RfConfig::SetPathProfile, this, _1))
        .set(sDefault);
}

void RfConfig::SetPgaGain( const double gain)
{
	DciProperty prop(m_idComponent, m_dci_ctrl, A2300_WAIT_TIME);
	byte idProperty = (m_bIsTx) ?  0x06 : 0x02;
	// FIXME?: ?should really verify that the range is within range?
	// FIXME?: ?should use A2300_RXGAIN_STEP or A2300_TXGAIN_STEP?
	byte val = ((byte)gain + 2) / 3;  // Round to closest step.
	prop.SetProperty<byte>(idProperty, val);
}

uhd::meta_range_t 	RfConfig::GetPgaGainRange(void)
{
	if( m_bIsTx)
		return uhd::meta_range_t( A2300_TXGAIN_MIN, A2300_TXGAIN_MAX, A2300_TXGAIN_STEP);
	else
		return uhd::meta_range_t( A2300_RXGAIN_MIN, A2300_RXGAIN_MAX, A2300_RXGAIN_STEP);
}

void RfConfig::SetRfBandwidth( const double bandwidth)
{
	int idxBw = GetBandwidthIndex( bandwidth);
	if( idxBw < 0 )
	{
        UHD_MSG(warning) << boost::format(
        	"BW Error: Value %.2lf not in range [%.2lf,%.2lf]\n")
        	%  bandwidth %  s_rfbandwidths[0] % s_rfbandwidths[CT_BANDWIDTHS -1];
		return;
	}

	//Submit the property.
	DciProperty prop(m_idComponent, m_dci_ctrl, A2300_WAIT_TIME);
	byte idProperty = (m_bIsTx) ?  0x09 : 0x05;
	byte val = (byte) idxBw;
	prop.SetProperty<byte>(idProperty, val);
}

uhd::meta_range_t 	RfConfig::GetRfBandwidthRange(void)
{
    meta_range_t range;
	for( size_t nn = 0; nn < CT_BANDWIDTHS; ++nn)
		range.push_back( range_t( s_rfbandwidths[nn]));
    return range;
}

int RfConfig::GetBandwidthIndex( const double bandwidth)
{
	// Verify range.
	if( (bandwidth < s_rfbandwidths[0]) ||
		(bandwidth > s_rfbandwidths[CT_BANDWIDTHS-1]) )
	{
		return -1;
	}

	// Find best match (below).
	int iFound = CT_BANDWIDTHS-1;
	while(s_rfbandwidths[--iFound] > bandwidth);
	return(iFound);
}

void RfConfig::SetRfFrequency( const double freq)
{
	DciProperty prop(m_idComponent, m_dci_ctrl, A2300_WAIT_TIME);
	byte idProperty = (m_bIsTx) ?  0x07 : 0x03;
	uint32 iFreqKHz = (uint32) (freq/ 1000.0);
	prop.SetProperty<uint32>(idProperty, iFreqKHz );
}

uhd::meta_range_t 	RfConfig::GetRfFrequencyRange(void)
{
	//TODO Change frequency Range based on Path Profile selection.
	return uhd::meta_range_t( A2300_RFFREQ_MIN, A2300_RFFREQ_MAX, A2300_RFFREQ_STEP);
}

void   RfConfig::SetPathProfile( const std::string& sPathId)
{
	byte idPath = 0xFF;
    if( m_idComponent == WCACOMP_RF0)
    {
    	if( m_bIsTx )
    	{
    		idPath = (sPathId[0] == 'W')? TX0DPE_Wideband : TX0DPE_Disabled;
    	}
    	else
    	{
    		if( sPathId == "L1GpsInt")		idPath = RX0DPE_GpsL1Int;
    		else if( sPathId == "L1GpsExt")	idPath = RX0DPE_GpsL1Ext;
    		else if( sPathId == "PcsExt")	idPath = RX0DPE_PcsExt;
    		else if( sPathId == "Wideband")	idPath = RX0DPE_Wideband;
    		else /*disabled*/				idPath = RX0DPE_Disabled;
    	}
    }
    else //Assume its WCACOMP_RF1
    {
    	if( m_bIsTx )
    	{
    		if( sPathId == "IsmInt")		idPath = TX1DPE_IsmInt;
    		else if( sPathId == "IsmExt")	idPath = TX1DPE_IsmExt;
    		else if( sPathId == "Wideband")	idPath = TX1DPE_Wideband;
    		else /*disabled*/				idPath = TX1DPE_Disabled;
    	}
    	else
    	{
    		if( sPathId == "UhfExt")		idPath = RX1DPE_UhfExt;
    		else if( sPathId == "IsmInt")	idPath = RX1DPE_IsmInt;
    		else if( sPathId == "IsmExt")	idPath = RX1DPE_IsmExt;
    		else if( sPathId == "Wideband")	idPath = RX1DPE_Wideband;
    		else /*disabled*/				idPath = RX1DPE_Disabled;
    	}
    }

	DciProperty prop(m_idComponent, m_dci_ctrl, A2300_WAIT_TIME);
	byte idProperty = (m_bIsTx) ?  0x0E : 0x0D;
	prop.SetProperty<byte>(idProperty, idPath);

}
