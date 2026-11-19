//! The little of H.264 the browser's decoder needs from us.
//!
//! The helper sends what x264 makes: Annex B (start codes), the parameter sets in front of every key
//! frame. Browsers' WebCodecs decoders all take the AVC form (each unit behind its 4-byte length,
//! the parameter sets in a separate description); only some take Annex B. So the phone turns each
//! frame into the AVC form, once, and the browser does nothing but decode.

/// The units in an Annex B buffer, start codes (and zero padding before them) taken off.
pub fn nal_units<'a>(data: &'a [u8]) -> Vec<&'a [u8]> {
    let mut out = Vec::new();
    let n = data.len();
    let mut start: Option<usize> = None;
    let mut i = 0;
    let close = |out: &mut Vec<&'a [u8]>, s: usize, mut e: usize| {
        // A unit never ends in a zero byte (its last bit is the stop bit): zeros there belong to
        // the next 4-byte start code.
        while e > s && data[e - 1] == 0 {
            e -= 1;
        }
        if e > s {
            out.push(&data[s..e]);
        }
    };
    while i + 2 < n {
        if data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 {
            if let Some(s) = start {
                close(&mut out, s, i);
            }
            i += 3;
            start = Some(i);
        } else if data[i + 2] > 1 {
            i += 3; // no start code can begin at i, i+1 or i+2
        } else {
            i += 1;
        }
    }
    if let Some(s) = start {
        close(&mut out, s, n);
    }
    out
}

pub const NAL_IDR: u8 = 5;
pub const NAL_SPS: u8 = 7;
pub const NAL_PPS: u8 = 8;
pub const NAL_AUD: u8 = 9;

pub fn nal_type(unit: &[u8]) -> u8 {
    unit[0] & 0x1f
}

/// `avc1.PPCCLL`: profile, constraint flags and level, from the SPS.
pub fn codec_string(sps: &[u8]) -> String {
    if sps.len() < 4 {
        return "avc1.42e01f".into();
    }
    format!("avc1.{:02x}{:02x}{:02x}", sps[1], sps[2], sps[3])
}

/// The AVCDecoderConfigurationRecord (ISO/IEC 14496-15) for one SPS and one PPS.
pub fn avcc(sps: &[u8], pps: &[u8]) -> Vec<u8> {
    let profile = sps.get(1).copied().unwrap_or(66);
    let high = matches!(profile, 100 | 110 | 122 | 144);
    let mut out = Vec::with_capacity(16 + sps.len() + pps.len());
    out.push(1);
    out.extend_from_slice(&sps[1..4.min(sps.len())]);
    while out.len() < 4 {
        out.push(0);
    }
    out.push(0xff); // 4-byte lengths
    out.push(0xe1); // one SPS
    out.extend_from_slice(&(sps.len() as u16).to_be_bytes());
    out.extend_from_slice(sps);
    out.push(1); // one PPS
    out.extend_from_slice(&(pps.len() as u16).to_be_bytes());
    out.extend_from_slice(pps);
    if high {
        out.extend_from_slice(&[0xfc | 1, 0xf8, 0xf8, 0]); // 4:2:0, 8 bit, no SPS extensions
    }
    out
}

/// One frame, ready for the browser.
pub struct Frame {
    pub sps: Option<Vec<u8>>,
    pub pps: Option<Vec<u8>>,
    /// An IDR is in it: decoding can start here.
    pub key: bool,
    /// The other units, each behind its length.
    pub avc: Vec<u8>,
}

pub fn convert(annex_b: &[u8]) -> Frame {
    let mut f = Frame { sps: None, pps: None, key: false, avc: Vec::with_capacity(annex_b.len() + 16) };
    for u in nal_units(annex_b) {
        match nal_type(u) {
            NAL_SPS => f.sps = Some(u.to_vec()),
            NAL_PPS => f.pps = Some(u.to_vec()),
            NAL_AUD => {}
            t => {
                if t == NAL_IDR {
                    f.key = true;
                }
                f.avc.extend_from_slice(&(u.len() as u32).to_be_bytes());
                f.avc.extend_from_slice(u);
            }
        }
    }
    f
}

/// The picture's size from an SPS, for the browser's first configure (it learns the real one from
/// the frames anyway). None for anything this doesn't follow.
pub fn sps_size(sps: &[u8]) -> Option<(u32, u32)> {
    // Emulation prevention bytes out first.
    let mut rbsp = Vec::with_capacity(sps.len());
    let mut zeros = 0;
    for &b in sps.get(1..)? {
        if zeros >= 2 && b == 3 {
            zeros = 0;
            continue;
        }
        zeros = if b == 0 { zeros + 1 } else { 0 };
        rbsp.push(b);
    }
    let mut r = Bits { d: &rbsp, p: 0 };
    let profile = r.u(8)?;
    r.u(16)?; // constraint flags + level
    r.ue()?; // sps id
    if matches!(profile, 100 | 110 | 122 | 244 | 44 | 83 | 86 | 118 | 128 | 138 | 139 | 134 | 135) {
        let chroma = r.ue()?;
        if chroma == 3 {
            r.u(1)?;
        }
        r.ue()?;
        r.ue()?;
        r.u(1)?;
        if r.u(1)? == 1 {
            return None; // scaling matrices: x264 never writes them for us
        }
    }
    r.ue()?; // log2_max_frame_num
    let poc = r.ue()?;
    if poc == 0 {
        r.ue()?;
    } else if poc == 1 {
        r.u(1)?;
        r.se()?;
        r.se()?;
        let n = r.ue()?;
        for _ in 0..n {
            r.se()?;
        }
    }
    r.ue()?; // max_num_ref_frames
    r.u(1)?;
    let w_mbs = r.ue()? + 1;
    let h_units = r.ue()? + 1;
    let frame_mbs_only = r.u(1)?;
    if frame_mbs_only == 0 {
        r.u(1)?;
    }
    r.u(1)?; // direct_8x8
    let (mut cl, mut cr, mut ct, mut cb) = (0, 0, 0, 0);
    if r.u(1)? == 1 {
        cl = r.ue()?;
        cr = r.ue()?;
        ct = r.ue()?;
        cb = r.ue()?;
    }
    let w = w_mbs * 16 - 2 * (cl + cr);
    let h = h_units * 16 * (2 - frame_mbs_only) - 2 * (2 - frame_mbs_only) * (ct + cb);
    Some((w, h))
}

struct Bits<'a> {
    d: &'a [u8],
    p: usize,
}

impl Bits<'_> {
    fn bit(&mut self) -> Option<u32> {
        let b = *self.d.get(self.p / 8)?;
        let v = (b >> (7 - self.p % 8)) & 1;
        self.p += 1;
        Some(v as u32)
    }
    fn u(&mut self, n: u32) -> Option<u32> {
        let mut v = 0;
        for _ in 0..n {
            v = v << 1 | self.bit()?;
        }
        Some(v)
    }
    fn ue(&mut self) -> Option<u32> {
        let mut zeros = 0;
        while self.bit()? == 0 {
            zeros += 1;
            if zeros > 31 {
                return None;
            }
        }
        Some((1u32 << zeros) - 1 + self.u(zeros)?)
    }
    fn se(&mut self) -> Option<i32> {
        let k = self.ue()?;
        Some(if k % 2 == 1 { (k as i32 + 1) / 2 } else { -(k as i32 / 2) })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // A real x264 SPS (1920x1080, constrained baseline 4.0) and PPS.
    const SPS: [u8; 11] = [0x67, 0x42, 0xc0, 0x28, 0xda, 0x01, 0xe0, 0x08, 0x9f, 0x96, 0x10];
    const PPS: [u8; 4] = [0x68, 0xce, 0x0f, 0xc8];

    #[test]
    fn splits_three_and_four_byte_start_codes() {
        let mut s = vec![0, 0, 0, 1];
        s.extend_from_slice(&SPS);
        s.extend_from_slice(&[0, 0, 0, 1]);
        s.extend_from_slice(&PPS);
        s.extend_from_slice(&[0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33, 0, 0]);
        let u = nal_units(&s);
        assert_eq!(u.len(), 3);
        assert_eq!(u[0], &SPS);
        assert_eq!(u[1], &PPS);
        assert_eq!(u[2], &[0x65, 0x88, 0x84, 0x00, 0x33]);
    }

    #[test]
    fn converts_to_avc() {
        let mut s = vec![0, 0, 0, 1, 0x09, 0xf0];
        s.extend_from_slice(&[0, 0, 0, 1]);
        s.extend_from_slice(&SPS);
        s.extend_from_slice(&[0, 0, 0, 1]);
        s.extend_from_slice(&PPS);
        s.extend_from_slice(&[0, 0, 0, 1, 0x65, 1, 2, 3]);
        let f = convert(&s);
        assert!(f.key);
        assert_eq!(f.sps.as_deref(), Some(&SPS[..]));
        assert_eq!(f.pps.as_deref(), Some(&PPS[..]));
        assert_eq!(f.avc, vec![0, 0, 0, 4, 0x65, 1, 2, 3]);
        let p = convert(&[0, 0, 0, 1, 0x41, 9, 9]);
        assert!(!p.key && p.sps.is_none());
        assert_eq!(p.avc, vec![0, 0, 0, 3, 0x41, 9, 9]);
    }

    #[test]
    fn codec_and_record() {
        assert_eq!(codec_string(&SPS), "avc1.42c028");
        let r = avcc(&SPS, &PPS);
        assert_eq!(&r[..6], &[1, 0x42, 0xc0, 0x28, 0xff, 0xe1]);
        assert_eq!(r.len(), 6 + 2 + SPS.len() + 1 + 2 + PPS.len());
    }

    #[test]
    fn size_from_sps() {
        assert_eq!(sps_size(&SPS), Some((1920, 1080)));
    }
}
