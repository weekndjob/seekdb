// Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0.
//! A bounded local Parquet decoder. SQL execution remains in the C++ engine.
mod page_guard;
use parquet::{
    basic::{LogicalType, Type as Physical},
    file::{reader::FileReader, serialized_reader::SerializedFileReader},
    record::{reader::RowIter, Field},
    schema::types::Type,
};
use std::{
    fs::File,
    io::{Read, Seek, SeekFrom},
    sync::Arc,
};

#[cxx::bridge(namespace = "oceanbase::file_sql")]
pub mod ffi {
    #[derive(Clone, Copy, PartialEq, Eq)]
    enum Kind {
        Null,
        Integer,
        Double,
        Boolean,
        Text,
        Date,
        Datetime,
    }
    struct Column {
        name: String,
        kind: Kind,
        nullable: bool,
    }
    struct Value {
        kind: Kind,
        integer: i64,
        floating: f64,
        text: String,
    }
    extern "Rust" {
        type ParquetScan;
        fn open_parquet(fd: i32, projection: &[u32], metadata_only: bool) -> Box<ParquetScan>;
        fn error(self: &ParquetScan) -> &str;
        fn columns(self: &ParquetScan) -> &[Column];
        // 0: row available; 1: end; -1: invalid/unsupported input.
        fn advance(self: &mut ParquetScan) -> i32;
        fn row(self: &ParquetScan) -> &[Value];
    }
}
pub struct ParquetScan {
    columns: Vec<ffi::Column>,
    iterator: Option<RowIter<'static>>,
    values: Vec<ffi::Value>,
    error: String,
}

const MAX_FOOTER: u64 = 16 * 1024 * 1024;
const MAX_GROUP_BYTES: i64 = 128 * 1024 * 1024;
const MAX_VALUE: usize = 16 * 1024 * 1024;

#[cfg(unix)]
fn clone_fd(fd: i32) -> Result<File, String> {
    use std::os::fd::FromRawFd;
    // Only the duplicated descriptor is owned by Rust. The secure path guard in
    // C++ keeps its original descriptor for mutation checks and scan lifetime.
    let duplicate = unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, 0) };
    if duplicate < 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    Ok(unsafe { File::from_raw_fd(duplicate) })
}
#[cfg(not(unix))]
fn clone_fd(_: i32) -> Result<File, String> {
    Err("local Parquet scans require Unix".into())
}

fn make_reader(
    mut file: File,
    projection: &[u32],
    metadata_only: bool,
) -> Result<ParquetScan, String> {
    let size = file.metadata().map_err(|e| e.to_string())?.len();
    if size < 12 {
        return Err("truncated Parquet file".into());
    }
    file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
    let mut marker = [0_u8; 4];
    file.read_exact(&mut marker).map_err(|e| e.to_string())?;
    if &marker != b"PAR1" {
        return Err("invalid Parquet header".into());
    }
    file.seek(SeekFrom::End(-8)).map_err(|e| e.to_string())?;
    let mut footer = [0_u8; 8];
    file.read_exact(&mut footer).map_err(|e| e.to_string())?;
    let footer_length = u64::from(u32::from_le_bytes([
        footer[0], footer[1], footer[2], footer[3],
    ]));
    if &footer[4..] != b"PAR1" || footer_length > MAX_FOOTER || footer_length > size - 12 {
        return Err("invalid or oversized Parquet footer".into());
    }
    file.seek(SeekFrom::Start(size - 8 - footer_length))
        .map_err(|e| e.to_string())?;
    let mut footer_bytes = vec![0; footer_length as usize];
    file.read_exact(&mut footer_bytes)
        .map_err(|e| e.to_string())?;
    page_guard::validate_footer(&footer_bytes)?;
    drop(footer_bytes);
    let mut preflight = file.try_clone().map_err(|e| e.to_string())?;
    let reader = SerializedFileReader::new(file).map_err(|e| e.to_string())?;
    let schema = reader.metadata().file_metadata().schema_descr();
    if schema.num_columns() == 0
        || schema.num_columns() > 1024
        || schema.root_schema().get_fields().len() != schema.num_columns()
    {
        return Err("Parquet requires 1..1024 flat scalar columns".into());
    }
    let mut columns = Vec::with_capacity(schema.num_columns());
    for column in schema.columns() {
        if column.max_rep_level() != 0
            || column.path().parts().len() != 1
            || column.name().is_empty()
            || column.name().len() > 1024
            || column.name().contains('\0')
        {
            return Err("nested or invalid Parquet column".into());
        }
        let logical = column.logical_type();
        let kind = match column.physical_type() {
            Physical::BOOLEAN => ffi::Kind::Boolean,
            Physical::INT32 if matches!(logical, Some(LogicalType::Date)) => ffi::Kind::Date,
            Physical::INT64 if matches!(logical, Some(LogicalType::Timestamp { .. })) => {
                ffi::Kind::Datetime
            }
            Physical::INT96 => ffi::Kind::Datetime,
            Physical::INT32 | Physical::INT64 => {
                if matches!(
                    logical,
                    Some(LogicalType::Decimal { .. } | LogicalType::Time { .. })
                ) {
                    return Err("unsupported Parquet numeric annotation".into());
                }
                if matches!(
                    logical,
                    Some(LogicalType::Integer {
                        bit_width: 64,
                        is_signed: false
                    })
                ) {
                    ffi::Kind::Text
                } else {
                    ffi::Kind::Integer
                }
            }
            Physical::FLOAT | Physical::DOUBLE => ffi::Kind::Double,
            Physical::BYTE_ARRAY | Physical::FIXED_LEN_BYTE_ARRAY => {
                if matches!(logical, Some(LogicalType::Decimal { .. })) {
                    return Err("Parquet decimals are outside the file SQL type set".into());
                }
                ffi::Kind::Text
            }
        };
        if columns
            .iter()
            .any(|c: &ffi::Column| c.name.eq_ignore_ascii_case(column.name()))
        {
            return Err("ambiguous duplicate Parquet columns".into());
        }
        columns.push(ffi::Column {
            name: column.name().to_owned(),
            kind,
            nullable: column.max_def_level() != 0,
        });
    }
    let selected: Vec<usize> = if projection.is_empty() {
        (0..columns.len()).collect()
    } else {
        projection.iter().map(|i| *i as usize).collect()
    };
    if selected.iter().any(|i| *i >= columns.len())
        || selected.windows(2).any(|pair| pair[0] >= pair[1])
    {
        return Err("invalid Parquet column projection".into());
    }
    if reader.num_row_groups() > 100_000 {
        return Err("too many Parquet row groups".into());
    }
    if metadata_only {
        return Ok(ParquetScan {
            columns,
            iterator: None,
            values: Vec::new(),
            error: String::new(),
        });
    }
    for group in reader.metadata().row_groups() {
        let mut total = 0_i64;
        for &i in &selected {
            let chunk = group.column(i);
            let uncompressed = chunk.uncompressed_size();
            let compressed = chunk.compressed_size();
            if uncompressed < 0 || compressed < 0 || compressed > MAX_GROUP_BYTES {
                return Err("invalid or oversized Parquet column chunk".into());
            }
            total = total
                .checked_add(uncompressed)
                .ok_or("Parquet size overflow")?;
            if total > MAX_GROUP_BYTES {
                return Err("Parquet row group exceeds scan memory bound".into());
            }
            let (start, chunk_length) = chunk.byte_range();
            page_guard::validate_pages(
                &mut preflight,
                start,
                chunk_length,
                size - 8 - footer_length,
            )?;
        }
    }
    let fields = selected
        .iter()
        .map(|i| Arc::clone(&schema.root_schema().get_fields()[*i]))
        .collect();
    let projected = Type::group_type_builder(schema.root_schema().name())
        .with_fields(fields)
        .build()
        .map_err(|e| e.to_string())?;
    let iterator = reader
        .into_iter()
        .project(Some(projected))
        .map_err(|e| e.to_string())?
        .with_batch_size(256);
    Ok(ParquetScan {
        columns,
        iterator: Some(iterator),
        values: Vec::new(),
        error: String::new(),
    })
}

pub fn open_parquet(fd: i32, projection: &[u32], metadata_only: bool) -> Box<ParquetScan> {
    Box::new(
        match clone_fd(fd).and_then(|file| make_reader(file, projection, metadata_only)) {
            Ok(reader) => reader,
            Err(error) => ParquetScan {
                columns: Vec::new(),
                iterator: None,
                values: Vec::new(),
                error,
            },
        },
    )
}
fn convert(field: Field) -> Result<ffi::Value, String> {
    let mut value = ffi::Value {
        kind: ffi::Kind::Null,
        integer: 0,
        floating: 0.0,
        text: String::new(),
    };
    match field {
        Field::Null => {}
        Field::Bool(v) => {
            value.kind = ffi::Kind::Boolean;
            value.integer = i64::from(v);
        }
        Field::Byte(v) => {
            value.kind = ffi::Kind::Integer;
            value.integer = i64::from(v);
        }
        Field::Short(v) => {
            value.kind = ffi::Kind::Integer;
            value.integer = i64::from(v);
        }
        Field::Int(v) => {
            value.kind = ffi::Kind::Integer;
            value.integer = i64::from(v);
        }
        Field::Long(v) => {
            value.kind = ffi::Kind::Integer;
            value.integer = v;
        }
        Field::UByte(v) => {
            value.kind = ffi::Kind::Integer;
            value.integer = i64::from(v);
        }
        Field::UShort(v) => {
            value.kind = ffi::Kind::Integer;
            value.integer = i64::from(v);
        }
        Field::UInt(v) => {
            value.kind = ffi::Kind::Integer;
            value.integer = i64::from(v);
        }
        Field::ULong(v) => {
            value.kind = ffi::Kind::Text;
            value.text = v.to_string();
        }
        Field::Float(v) => {
            value.kind = ffi::Kind::Double;
            value.floating = f64::from(v);
        }
        Field::Double(v) => {
            value.kind = ffi::Kind::Double;
            value.floating = v;
        }
        Field::Date(v) => {
            value.kind = ffi::Kind::Date;
            value.integer = i64::from(v);
        }
        Field::TimestampMillis(v) => {
            value.kind = ffi::Kind::Datetime;
            value.integer = v.checked_mul(1000).ok_or("Parquet timestamp overflow")?;
        }
        Field::TimestampMicros(v) => {
            value.kind = ffi::Kind::Datetime;
            value.integer = v;
        }
        Field::Str(v) => {
            value.kind = ffi::Kind::Text;
            value.text = v;
        }
        Field::Bytes(v) => {
            value.kind = ffi::Kind::Text;
            if v.data().len() > MAX_VALUE {
                return Err("oversized Parquet value".into());
            }
            value.text = std::str::from_utf8(v.data())
                .map_err(|e| e.to_string())?
                .to_owned();
        }
        _ => return Err("unsupported nested or decimal Parquet value".into()),
    }
    if !value.floating.is_finite() || value.text.len() > MAX_VALUE {
        return Err("non-finite or oversized Parquet value".into());
    }
    Ok(value)
}
impl ParquetScan {
    pub fn error(&self) -> &str {
        &self.error
    }
    pub fn columns(&self) -> &[ffi::Column] {
        &self.columns
    }
    pub fn row(&self) -> &[ffi::Value] {
        &self.values
    }
    pub fn advance(&mut self) -> i32 {
        self.values.clear();
        if !self.error.is_empty() {
            return -1;
        }
        let row = match self.iterator.as_mut().and_then(Iterator::next) {
            None => return 1,
            Some(Ok(row)) => row,
            Some(Err(error)) => {
                self.error = error.to_string();
                return -1;
            }
        };
        for (_, field) in row.into_columns() {
            match convert(field) {
                Ok(value) => self.values.push(value),
                Err(error) => {
                    self.values.clear();
                    self.error = error;
                    return -1;
                }
            }
        }
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use parquet::{
        basic::Compression,
        data_type::{DoubleType, Int64Type},
        file::{properties::WriterProperties, writer::SerializedFileWriter},
        schema::parser::parse_message_type,
    };
    use std::{
        fs::OpenOptions,
        io::Write,
        path::PathBuf,
        sync::atomic::{AtomicU64, Ordering},
    };
    static SEQUENCE: AtomicU64 = AtomicU64::new(0);
    struct Fixture(PathBuf);
    impl Fixture {
        fn new() -> Self {
            Self(std::env::temp_dir().join(format!(
                "seekdb-parquet-{}-{}.parquet",
                std::process::id(),
                SEQUENCE.fetch_add(1, Ordering::Relaxed)
            )))
        }
        fn create(&self) -> File {
            OpenOptions::new()
                .create_new(true)
                .write(true)
                .open(&self.0)
                .unwrap()
        }
        fn read(&self) -> File {
            File::open(&self.0).unwrap()
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }
    fn rows() -> Fixture {
        let fixture = Fixture::new();
        let schema = Arc::new(
            parse_message_type("message f { REQUIRED INT64 id; OPTIONAL DOUBLE score; }").unwrap(),
        );
        let properties = Arc::new(
            WriterProperties::builder()
                .set_compression(Compression::SNAPPY)
                .build(),
        );
        let mut writer = SerializedFileWriter::new(fixture.create(), schema, properties).unwrap();
        for base in [0_i64, 3] {
            let mut group = writer.next_row_group().unwrap();
            let mut column = group.next_column().unwrap().unwrap();
            column
                .typed::<Int64Type>()
                .write_batch(&[base + 1, base + 2, base + 3], None, None)
                .unwrap();
            column.close().unwrap();
            let mut column = group.next_column().unwrap().unwrap();
            column
                .typed::<DoubleType>()
                .write_batch(&[1.5, 3.5], Some(&[1, 0, 1]), None)
                .unwrap();
            column.close().unwrap();
            group.close().unwrap();
        }
        writer.close().unwrap();
        fixture
    }
    #[test]
    fn streams_multiple_groups_with_nulls_and_projection() {
        let fixture = rows();
        let mut full = make_reader(fixture.read(), &[], false).unwrap();
        assert_eq!(full.columns().len(), 2);
        assert!(full.columns()[1].nullable);
        for id in 1..=6 {
            assert_eq!(full.advance(), 0, "{}", full.error());
            assert_eq!(full.row()[0].integer, id);
            assert_eq!(full.row()[1].kind == ffi::Kind::Null, id == 2 || id == 5);
        }
        assert_eq!(full.advance(), 1);
        let mut projected = make_reader(fixture.read(), &[1], false).unwrap();
        assert_eq!(projected.advance(), 0);
        assert_eq!(projected.row().len(), 1);
        assert_eq!(projected.row()[0].floating, 1.5);
        assert!(make_reader(fixture.read(), &[2], false).is_err());
        assert!(make_reader(fixture.read(), &[1, 0], false).is_err());
    }
    #[test]
    fn rejects_invalid_footer_and_nested_schema() {
        let bad = Fixture::new();
        bad.create().write_all(b"PAR1\xff\xff\xff\xffPAR1").unwrap();
        assert!(make_reader(bad.read(), &[], false).is_err());
        let nested = Fixture::new();
        let schema = Arc::new(parse_message_type("message f { REPEATED INT64 ids; }").unwrap());
        SerializedFileWriter::new(nested.create(), schema, Default::default())
            .unwrap()
            .close()
            .unwrap();
        assert!(make_reader(nested.read(), &[], false).is_err());
    }
    #[test]
    fn conversion_rejects_non_finite_and_preserves_large_unsigned_integers() {
        assert!(convert(Field::Double(f64::NAN)).is_err());
        assert!(convert(Field::TimestampMillis(i64::MAX)).is_err());
        assert_eq!(
            convert(Field::ULong(u64::MAX)).unwrap().text,
            "18446744073709551615"
        );
    }
    #[test]
    fn footer_inference_and_projection_do_not_decode_unselected_columns() {
        let fixture = rows();
        let reader = SerializedFileReader::new(fixture.read()).unwrap();
        let (start, length) = reader.metadata().row_group(0).column(1).byte_range();
        drop(reader);
        let mut damaged = OpenOptions::new().write(true).open(&fixture.0).unwrap();
        damaged.seek(SeekFrom::Start(start)).unwrap();
        damaged.write_all(&vec![0; length as usize]).unwrap();
        drop(damaged);
        let metadata = make_reader(fixture.read(), &[], true).unwrap();
        assert_eq!(metadata.columns().len(), 2);
        let mut projected = make_reader(fixture.read(), &[0], false).unwrap();
        for id in 1..=6 {
            assert_eq!(projected.advance(), 0);
            assert_eq!(projected.row()[0].integer, id);
        }
        assert_eq!(projected.advance(), 1);
        assert!(make_reader(fixture.read(), &[], false).is_err());
    }
    #[test]
    fn supported_compression_codecs_roundtrip() {
        for codec in [
            Compression::UNCOMPRESSED,
            Compression::SNAPPY,
            Compression::GZIP(Default::default()),
            Compression::BROTLI(Default::default()),
            Compression::LZ4_RAW,
            Compression::ZSTD(Default::default()),
        ] {
            let fixture = Fixture::new();
            let schema = Arc::new(parse_message_type("message f { REQUIRED INT64 id; }").unwrap());
            let properties = Arc::new(WriterProperties::builder().set_compression(codec).build());
            let mut writer =
                SerializedFileWriter::new(fixture.create(), schema, properties).unwrap();
            let mut group = writer.next_row_group().unwrap();
            let mut column = group.next_column().unwrap().unwrap();
            column
                .typed::<Int64Type>()
                .write_batch(&[1, 2, 3], None, None)
                .unwrap();
            column.close().unwrap();
            group.close().unwrap();
            writer.close().unwrap();
            let mut scan = make_reader(fixture.read(), &[], false).unwrap();
            for expected in 1..=3 {
                assert_eq!(scan.advance(), 0, "{:?}: {}", codec, scan.error());
                assert_eq!(scan.row()[0].integer, expected);
            }
            assert_eq!(scan.advance(), 1);
        }
    }
}
