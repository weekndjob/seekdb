//! Validate allocation-driving page sizes before handing pages to the decoder.
use std::{
    fs::File,
    io::{Read, Seek, SeekFrom},
};
const HEADER_LIMIT: usize = 64 * 1024;
const PAGE_LIMIT: u64 = 64 * 1024 * 1024;

struct Compact<'a> {
    bytes: &'a [u8],
    pos: usize,
    dictionary_values: Option<u64>,
    allocation_budget: usize,
}
impl Compact<'_> {
    fn byte(&mut self) -> Result<u8, String> {
        let value = *self
            .bytes
            .get(self.pos)
            .ok_or("truncated Parquet page header")?;
        self.pos += 1;
        Ok(value)
    }
    fn take(&mut self, length: usize) -> Result<(), String> {
        self.pos = self
            .pos
            .checked_add(length)
            .filter(|p| *p <= self.bytes.len())
            .ok_or("invalid page header length")?;
        Ok(())
    }
    fn varint(&mut self) -> Result<u64, String> {
        let mut result = 0_u64;
        for shift in (0..70).step_by(7) {
            let byte = self.byte()?;
            if shift == 63 && byte > 1 {
                return Err("page header integer overflow".into());
            }
            result |= u64::from(byte & 127) << shift;
            if byte & 128 == 0 {
                return Ok(result);
            }
        }
        Err("invalid page header varint".into())
    }
    fn skip(&mut self, kind: u8, depth: usize, collection: bool) -> Result<(), String> {
        if depth > 16 {
            return Err("page header nesting limit".into());
        }
        match kind {
            1 | 2 if collection => {
                self.byte()?;
            }
            1 | 2 => {}
            3 => {
                self.byte()?;
            }
            4..=6 => {
                self.varint()?;
            }
            7 => self.take(8)?,
            8 => {
                let length =
                    usize::try_from(self.varint()?).map_err(|_| "page header size overflow")?;
                self.take(length)?;
            }
            9 | 10 => {
                let header = self.byte()?;
                let count = if header >> 4 == 15 {
                    self.varint()?
                } else {
                    u64::from(header >> 4)
                };
                if count > HEADER_LIMIT as u64 || count > self.allocation_budget as u64 {
                    return Err("Parquet metadata collection limit".into());
                }
                self.allocation_budget -= count as usize;
                for _ in 0..count {
                    self.skip(header & 15, depth + 1, true)?;
                }
            }
            11 => {
                let count = self.varint()?;
                if count > HEADER_LIMIT as u64 || count > (self.allocation_budget / 2) as u64 {
                    return Err("Parquet metadata map limit".into());
                }
                self.allocation_budget -= 2 * count as usize;
                if count != 0 {
                    let kinds = self.byte()?;
                    for _ in 0..count {
                        self.skip(kinds >> 4, depth + 1, true)?;
                        self.skip(kinds & 15, depth + 1, true)?;
                    }
                }
            }
            12 => {
                self.structure(depth + 1, false)?;
            }
            _ => return Err("invalid compact page header type".into()),
        }
        Ok(())
    }
    fn dictionary(&mut self) -> Result<(), String> {
        let mut field = 0_i64;
        loop {
            let header = self.byte()?;
            if header == 0 {
                break;
            }
            field = if header >> 4 == 0 {
                let encoded = self.varint()?;
                ((encoded >> 1) as i64) ^ -((encoded & 1) as i64)
            } else {
                field + i64::from(header >> 4)
            };
            if !(1..=32767).contains(&field) {
                return Err("invalid dictionary field".into());
            }
            if field == 1 {
                if header & 15 != 5 {
                    return Err("invalid dictionary count type".into());
                }
                let encoded = self.varint()?;
                let count = ((encoded >> 1) as i64) ^ -((encoded & 1) as i64);
                // The codec allocates a typed dictionary before decoding it.
                // Bound this allocation independently of compressed page size.
                if !(0..=4_194_304).contains(&count) {
                    return Err("Parquet dictionary exceeds memory bound".into());
                }
                self.dictionary_values = Some(count as u64);
            } else {
                self.skip(header & 15, 1, false)?;
            }
        }
        if self.dictionary_values.is_none() {
            return Err("missing dictionary count".into());
        }
        Ok(())
    }
    fn structure(&mut self, depth: usize, root: bool) -> Result<(u64, u64), String> {
        let mut field = 0_i64;
        let mut compressed = None;
        let mut uncompressed = None;
        loop {
            let header = self.byte()?;
            if header == 0 {
                break;
            }
            if self.allocation_budget == 0 {
                return Err("Parquet metadata allocation limit".into());
            }
            self.allocation_budget -= 1;
            field = if header >> 4 == 0 {
                let encoded = self.varint()?;
                ((encoded >> 1) as i64) ^ -((encoded & 1) as i64)
            } else {
                field
                    .checked_add(i64::from(header >> 4))
                    .ok_or("page field overflow")?
            };
            if !(1..=32767).contains(&field) {
                return Err("invalid page header field id".into());
            }
            if root && field == 7 {
                if header & 15 != 12 {
                    return Err("invalid dictionary header type".into());
                }
                self.dictionary()?;
            } else if root && (field == 2 || field == 3) {
                if header & 15 != 5 {
                    return Err("invalid Parquet page size type".into());
                }
                let encoded = self.varint()?;
                let size = ((encoded >> 1) as i64) ^ -((encoded & 1) as i64);
                if !(0..=PAGE_LIMIT as i64).contains(&size) {
                    return Err("Parquet page exceeds memory bound".into());
                }
                if field == 2 {
                    uncompressed = Some(size as u64);
                } else {
                    compressed = Some(size as u64);
                }
            } else {
                self.skip(header & 15, depth, false)?;
            }
        }
        if root {
            if self
                .dictionary_values
                .zip(uncompressed)
                .is_some_and(|(count, bytes)| count > bytes)
            {
                return Err("dictionary count exceeds encoded page contents".into());
            }
            Ok((
                uncompressed.ok_or("missing Parquet page size")?,
                compressed.ok_or("missing Parquet page size")?,
            ))
        } else {
            Ok((0, 0))
        }
    }
}

// Scan the compact footer without allocations before the library builds its
// schema/row-group vectors. A short malicious list cannot reserve huge memory.
pub fn validate_footer(bytes: &[u8]) -> Result<(), String> {
    let mut parser = Compact {
        bytes,
        pos: 0,
        dictionary_values: None,
        allocation_budget: 131072,
    };
    parser.structure(0, false)?;
    if parser.pos != bytes.len() {
        return Err("trailing Parquet footer bytes".into());
    }
    Ok(())
}

pub fn validate_pages(
    file: &mut File,
    start: u64,
    length: u64,
    file_limit: u64,
) -> Result<(), String> {
    let end = start
        .checked_add(length)
        .filter(|end| start >= 4 && *end <= file_limit)
        .ok_or("Parquet column chunk outside data section")?;
    let mut offset = start;
    let mut buffer = vec![0_u8; HEADER_LIMIT];
    let mut pages = 0_usize;
    while offset < end {
        pages += 1;
        if pages > 1_000_000 {
            return Err("Parquet page count limit".into());
        }
        let count = usize::try_from((end - offset).min(HEADER_LIMIT as u64))
            .map_err(|_| "page size overflow")?;
        file.seek(SeekFrom::Start(offset))
            .map_err(|e| e.to_string())?;
        file.read_exact(&mut buffer[..count])
            .map_err(|e| e.to_string())?;
        let mut parser = Compact {
            bytes: &buffer[..count],
            pos: 0,
            dictionary_values: None,
            allocation_budget: 131072,
        };
        let (_, compressed) = parser.structure(0, true)?;
        offset = offset
            .checked_add(parser.pos as u64)
            .and_then(|p| p.checked_add(compressed))
            .filter(|p| *p <= end)
            .ok_or("truncated Parquet page body")?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn validates_size_headers_without_allocating_advertised_sizes() {
        let mut valid = Compact {
            bytes: &[0x15, 0, 0x15, 16, 0x15, 16, 0],
            pos: 0,
            dictionary_values: None,
            allocation_budget: 131072,
        };
        assert_eq!(valid.structure(0, true).unwrap(), (8, 8));
        let dictionary = [
            0x15, 4, 0x15, 16, 0x15, 16, 0x4c, 0x15, 0xfe, 0xff, 0xff, 0xff, 0x0f, 0, 0,
        ];
        assert!(Compact {
            bytes: &dictionary,
            pos: 0,
            dictionary_values: None,
            allocation_budget: 131072
        }
        .structure(0, true)
        .is_err());
        let huge = [0x15, 0, 0x15, 0xfe, 0xff, 0xff, 0xff, 0x0f, 0x15, 0, 0];
        assert!(Compact {
            bytes: &huge,
            pos: 0,
            dictionary_values: None,
            allocation_budget: 131072
        }
        .structure(0, true)
        .is_err());
        assert!(Compact {
            bytes: &[0x18, 127],
            pos: 0,
            dictionary_values: None,
            allocation_budget: 131072
        }
        .structure(0, true)
        .is_err());
    }
    #[test]
    fn rejects_footer_allocation_bombs_before_metadata_decode() {
        // FileMetaData.schema is a compact list. Its count advertises billions
        // of objects while the actual input contains no such objects.
        assert!(validate_footer(&[0x29, 0xfc, 0xff, 0xff, 0xff, 0x7f, 0]).is_err());
        assert!(validate_footer(&[0]).is_ok());
        assert!(validate_footer(&[0, 0]).is_err());
    }
}
