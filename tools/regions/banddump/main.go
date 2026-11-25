// Dump the RP002 tables of github.com/brocaar/lorawan/band (MIT) as JSON, per
// region and protocol version: the reference for the gateway's C tables.
package main

import (
	"encoding/json"
	"os"

	"github.com/brocaar/lorawan"
	"github.com/brocaar/lorawan/band"
)

type dr struct {
	DR  int    `json:"dr"`
	Mod string `json:"mod"`
	SF  int    `json:"sf"`
	BW  int    `json:"bw"`
	Bps int    `json:"bps"`
	M   int    `json:"m"`
	N   int    `json:"n"`
}

type ch struct {
	Freq  uint32 `json:"freq"`
	MinDR int    `json:"min_dr"`
	MaxDR int    `json:"max_dr"`
}

type out struct {
	Name         string      `json:"name"`
	Proto        string      `json:"proto"`
	Rev          string      `json:"rev"`
	Dwell        bool        `json:"dwell"`
	Repeater     bool        `json:"repeater"`
	DRs          []dr        `json:"drs"`
	RX1DR        [][]int     `json:"rx1_dr"` // [uplink dr][offset], -1: none
	TXPower      []int       `json:"tx_power_offset"`
	Up           []ch        `json:"uplink"`
	Down         []ch        `json:"downlink"`
	RX1ChanFor   []int       `json:"rx1_channel_for_uplink"`
	RX2Freq      uint32      `json:"rx2_freq"`
	RX2DR        int         `json:"rx2_dr"`
	RX1Delay     int         `json:"rx1_delay_s"`
	JoinDelay1   int         `json:"join_accept_delay1_s"`
	MaxEIRP      float32     `json:"max_uplink_eirp"`
	TXParamSetup bool        `json:"tx_param_setup"`
	CFList       interface{} `json:"cflist"`
	SubBands     []sub       `json:"sub_bands,omitempty"`
}

type sub struct {
	SubBand int         `json:"sub_band"`
	CFList  interface{} `json:"cflist"`
	LinkADR interface{} `json:"link_adr"`
}

func main() {
	regions := []band.Name{band.EU868, band.US915, band.AU915, band.AS923, band.AS923_2, band.AS923_3, band.AS923_4,
		band.IN865, band.KR920, band.RU864, band.CN470, band.EU433}
	var all []out
	for _, proto := range []string{band.LoRaWAN_1_0_2, band.LoRaWAN_1_0_4} {
		rev := band.RegParamRevB
		if proto == band.LoRaWAN_1_0_4 {
			rev = band.RegParamRevRP002_1_0_3
		}
		for _, name := range regions {
			for _, rep := range []bool{false, true} {
				for _, dwell := range []bool{false, true} {
					dt := lorawan.DwellTimeNoLimit
					if dwell {
						dt = lorawan.DwellTime400ms
					}
					b, err := band.GetConfig(name, rep, dt)
					if err != nil {
						continue
					}
					o := out{Name: string(name), Proto: proto, Rev: rev, Dwell: dwell, Repeater: rep}
					for i := 0; i < 16; i++ {
						d, err := b.GetDataRate(i)
						if err != nil {
							continue
						}
						m, err := b.GetMaxPayloadSizeForDataRateIndex(proto, rev, i)
						if err != nil {
							m = band.MaxPayloadSize{}
						}
						o.DRs = append(o.DRs, dr{i, string(d.Modulation), d.SpreadFactor, d.Bandwidth, d.BitRate,
							m.M, m.N})
					}
					for i := 0; i < 16; i++ {
						row := []int{}
						for off := 0; off < 8; off++ {
							r, err := b.GetRX1DataRateIndex(i, off)
							if err != nil {
								r = -1
							}
							row = append(row, r)
						}
						o.RX1DR = append(o.RX1DR, row)
					}
					for p := 0; p < 16; p++ {
						v, err := b.GetTXPowerOffset(p)
						if err != nil {
							break
						}
						o.TXPower = append(o.TXPower, v)
					}
					for _, i := range b.GetUplinkChannelIndices() {
						c, _ := b.GetUplinkChannel(i)
						o.Up = append(o.Up, ch{c.Frequency, c.MinDR, c.MaxDR})
						rc, err := b.GetRX1ChannelIndexForUplinkChannelIndex(i)
						if err != nil {
							rc = -1
						}
						o.RX1ChanFor = append(o.RX1ChanFor, rc)
					}
					for i := 0; ; i++ {
						c, err := b.GetDownlinkChannel(i)
						if err != nil {
							break
						}
						o.Down = append(o.Down, ch{c.Frequency, c.MinDR, c.MaxDR})
					}
					def := b.GetDefaults()
					o.RX2Freq, o.RX2DR = def.RX2Frequency, def.RX2DataRate
					o.RX1Delay = int(def.ReceiveDelay1.Seconds())
					o.JoinDelay1 = int(def.JoinAcceptDelay1.Seconds())
					o.MaxEIRP = b.GetDefaultMaxUplinkEIRP()
					o.TXParamSetup = b.ImplementsTXParamSetup(proto)
					o.CFList = b.GetCFList(proto)
					if name == band.US915 || name == band.AU915 {
						all72 := b.GetUplinkChannelIndices()
						for sb := 0; sb < 8; sb++ {
							bs, _ := band.GetConfig(name, rep, dt)
							for _, i := range bs.GetUplinkChannelIndices() {
								_ = bs.DisableUplinkChannelIndex(i)
							}
							for i := sb * 8; i < sb*8+8; i++ {
								_ = bs.EnableUplinkChannelIndex(i)
							}
							_ = bs.EnableUplinkChannelIndex(64 + sb)
							o.SubBands = append(o.SubBands, sub{sb, bs.GetCFList(proto),
								bs.GetLinkADRReqPayloadsForEnabledUplinkChannelIndices(all72)})
						}
					}
					all = append(all, o)
				}
			}
		}
	}
	e := json.NewEncoder(os.Stdout)
	e.SetIndent("", " ")
	_ = e.Encode(all)
}
