package main

import (
	"encoding/hex"
	"errors"
	"strconv"
	"strings"

	"github.com/pion/sdp/v3"
	"github.com/pion/webrtc/v4"
)

// RFC 6184 table 5: ignore only the explicitly interchangeable constraint bits.
func h264Subprofile(p []byte) string {
	switch p[0] {
	case 0x42:
		if p[1]&0x4f == 0x40 {
			return "constrained-baseline"
		}
		if p[1]&0x4f == 0 {
			return "baseline"
		}
	case 0x4d:
		if p[1]&0x8f == 0x80 {
			return "constrained-baseline"
		}
		if p[1]&0xaf == 0 {
			return "main"
		}
	case 0x58:
		switch p[1] & 0xcf {
		case 0xc0:
			return "constrained-baseline"
		case 0x80:
			return "baseline"
		case 0:
			return "extended"
		}
	}
	return hex.EncodeToString(p[:2])
}
func h264Level(p []byte) int {
	if (p[0] == 0x42 || p[0] == 0x4d || p[0] == 0x58) && p[2] == 11 && p[1]&0x10 != 0 {
		return 105
	}
	return int(p[2]) * 10
}
func offeredH264(text, source string) (webrtc.RTPCodecParameters, error) {
	var empty webrtc.RTPCodecParameters
	actual, err := hex.DecodeString(source)
	if err != nil || len(actual) != 3 {
		return empty, errors.New("invalid source profile")
	}
	var description sdp.SessionDescription
	if description.Unmarshal([]byte(text)) != nil || len(description.MediaDescriptions) != 1 {
		return empty, errors.New("invalid offer")
	}
	media := description.MediaDescriptions[0]
	maps := make(map[string]string)
	formats := make(map[string]string)
	for _, a := range media.Attributes {
		if a.Key != "rtpmap" && a.Key != "fmtp" {
			continue
		}
		pair := strings.SplitN(a.Value, " ", 2)
		if len(pair) != 2 {
			continue
		}
		if a.Key == "rtpmap" {
			maps[pair[0]] = pair[1]
		} else {
			formats[pair[0]] = pair[1]
		}
	}
	for _, pt := range media.MediaName.Formats {
		if !strings.EqualFold(maps[pt], "H264/90000") {
			continue
		}
		parameters := make(map[string]string)
		for _, part := range strings.Split(formats[pt], ";") {
			pair := strings.SplitN(strings.TrimSpace(part), "=", 2)
			if len(pair) == 2 {
				parameters[pair[0]] = pair[1]
			}
		}
		if parameters["packetization-mode"] != "1" {
			continue
		}
		remote, e := hex.DecodeString(parameters["profile-level-id"])
		if e != nil || len(remote) != 3 || h264Subprofile(remote) != h264Subprofile(actual) || h264Level(actual) > h264Level(remote) {
			continue
		}
		payload, e := strconv.ParseUint(pt, 10, 7)
		if e != nil {
			continue
		}
		var feedback []webrtc.RTCPFeedback
		for _, attribute := range media.Attributes {
			fields := strings.Fields(attribute.Value)
			if attribute.Key != "rtcp-fb" || len(fields) < 2 || len(fields) > 3 || (fields[0] != pt && fields[0] != "*") { continue }
			if fields[1] == "nack" && (len(fields) == 2 || fields[2] == "pli") {
				entry := webrtc.RTCPFeedback{Type: "nack"}
				if len(fields) == 3 { entry.Parameter = fields[2] }
				feedback = append(feedback, entry)
			}
		}
		return webrtc.RTPCodecParameters{RTPCodecCapability: webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000, SDPFmtpLine: formats[pt], RTCPFeedback: feedback}, PayloadType: webrtc.PayloadType(payload)}, nil
	}
	return empty, errors.New("receiver does not support source H.264 profile/level and packetization mode")
}
