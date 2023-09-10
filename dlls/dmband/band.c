/* IDirectMusicBand Implementation
 *
 * Copyright (C) 2003-2004 Rok Mandeljc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dmband_private.h"
#include "dmobject.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmband);
WINE_DECLARE_DEBUG_CHANNEL(dmfile);

struct instrument_entry
{
    struct list entry;
    DMUS_IO_INSTRUMENT instrument;
    IDirectMusicCollection *collection;
};

static void instrument_entry_destroy(struct instrument_entry *entry)
{
    if (entry->collection) IDirectMusicCollection_Release(entry->collection);
    free(entry);
}

struct band
{
    IDirectMusicBand IDirectMusicBand_iface;
    struct dmobject dmobj;
    LONG ref;
    struct list instruments;
};

static inline struct band *impl_from_IDirectMusicBand(IDirectMusicBand *iface)
{
    return CONTAINING_RECORD(iface, struct band, IDirectMusicBand_iface);
}

static HRESULT WINAPI band_QueryInterface(IDirectMusicBand *iface, REFIID riid,
        void **ret_iface)
{
    struct band *This = impl_from_IDirectMusicBand(iface);

    TRACE("(%p, %s, %p)\n", This, debugstr_dmguid(riid), ret_iface);

    *ret_iface = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicBand))
        *ret_iface = iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject))
        *ret_iface = &This->dmobj.IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream))
        *ret_iface = &This->dmobj.IPersistStream_iface;
    else {
        WARN("(%p, %s, %p): not found\n", This, debugstr_dmguid(riid), ret_iface);
        return E_NOINTERFACE;
    }

    IDirectMusicBand_AddRef((IUnknown*)*ret_iface);
    return S_OK;
}

static ULONG WINAPI band_AddRef(IDirectMusicBand *iface)
{
    struct band *This = impl_from_IDirectMusicBand(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI band_Release(IDirectMusicBand *iface)
{
    struct band *This = impl_from_IDirectMusicBand(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if (!ref)
    {
        struct instrument_entry *entry, *next;

        LIST_FOR_EACH_ENTRY_SAFE(entry, next, &This->instruments, struct instrument_entry, entry)
        {
            list_remove(&entry->entry);
            instrument_entry_destroy(entry);
        }

        free(This);
    }

    return ref;
}

static HRESULT WINAPI band_CreateSegment(IDirectMusicBand *iface,
        IDirectMusicSegment **segment)
{
    struct band *This = impl_from_IDirectMusicBand(iface);
    HRESULT hr;
    DMUS_BAND_PARAM bandparam;

    FIXME("(%p, %p): semi-stub\n", This, segment);

    hr = CoCreateInstance(&CLSID_DirectMusicSegment, NULL, CLSCTX_INPROC,
            &IID_IDirectMusicSegment, (void**)segment);
    if (FAILED(hr))
        return hr;

    bandparam.mtTimePhysical = 0;
    bandparam.pBand = &This->IDirectMusicBand_iface;
    IDirectMusicBand_AddRef(bandparam.pBand);
    hr = IDirectMusicSegment_SetParam(*segment, &GUID_BandParam, 0xffffffff, DMUS_SEG_ALLTRACKS,
            0, &bandparam);
    IDirectMusicBand_Release(bandparam.pBand);

    return hr;
}

static HRESULT WINAPI band_Download(IDirectMusicBand *iface,
        IDirectMusicPerformance *pPerformance)
{
        struct band *This = impl_from_IDirectMusicBand(iface);
	FIXME("(%p, %p): stub\n", This, pPerformance);
	return S_OK;
}

static HRESULT WINAPI band_Unload(IDirectMusicBand *iface,
        IDirectMusicPerformance *pPerformance)
{
        struct band *This = impl_from_IDirectMusicBand(iface);
	FIXME("(%p, %p): stub\n", This, pPerformance);
	return S_OK;
}

static const IDirectMusicBandVtbl band_vtbl =
{
    band_QueryInterface,
    band_AddRef,
    band_Release,
    band_CreateSegment,
    band_Download,
    band_Unload,
};

static HRESULT parse_lbin_list(struct band *This, IStream *stream, struct chunk_entry *parent)
{
    struct chunk_entry chunk = {.parent = parent};
    IDirectMusicCollection *collection = NULL;
    struct instrument_entry *entry;
    DMUS_IO_INSTRUMENT inst = {0};
    HRESULT hr;

    while ((hr = stream_next_chunk(stream, &chunk)) == S_OK)
    {
        switch (MAKE_IDTYPE(chunk.id, chunk.type))
        {
        case DMUS_FOURCC_INSTRUMENT_CHUNK:
        {
            UINT size = sizeof(inst);

            if (chunk.size == offsetof(DMUS_IO_INSTRUMENT, nPitchBendRange)) size = chunk.size;
            if (FAILED(hr = stream_chunk_get_data(stream, &chunk, &inst, size))) break;
            TRACE_(dmfile)(" - dwPatch: %lu\n", inst.dwPatch);
            TRACE_(dmfile)(" - dwAssignPatch: %lu\n", inst.dwAssignPatch);
            TRACE_(dmfile)(" - dwNoteRanges[0]: %lu\n", inst.dwNoteRanges[0]);
            TRACE_(dmfile)(" - dwNoteRanges[1]: %lu\n", inst.dwNoteRanges[1]);
            TRACE_(dmfile)(" - dwNoteRanges[2]: %lu\n", inst.dwNoteRanges[2]);
            TRACE_(dmfile)(" - dwNoteRanges[3]: %lu\n", inst.dwNoteRanges[3]);
            TRACE_(dmfile)(" - dwPChannel: %lu\n", inst.dwPChannel);
            TRACE_(dmfile)(" - dwFlags: %lx\n", inst.dwFlags);
            TRACE_(dmfile)(" - bPan: %u\n", inst.bPan);
            TRACE_(dmfile)(" - bVolume: %u\n", inst.bVolume);
            TRACE_(dmfile)(" - nTranspose: %d\n", inst.nTranspose);
            TRACE_(dmfile)(" - dwChannelPriority: %lu\n", inst.dwChannelPriority);
            TRACE_(dmfile)(" - nPitchBendRange: %d\n", inst.nPitchBendRange);
            break;
        }

        case MAKE_IDTYPE(FOURCC_LIST, DMUS_FOURCC_REF_LIST):
        {
            IDirectMusicObject *object;

            if (FAILED(hr = dmobj_parsereference(stream, &chunk, &object))) break;
            TRACE_(dmfile)(" - object: %p\n", object);

            hr = IDirectMusicObject_QueryInterface(object, &IID_IDirectMusicCollection, (void **)&collection);
            IDirectMusicObject_Release(object);
            break;
        }

        default:
            FIXME("Ignoring chunk %s %s\n", debugstr_fourcc(chunk.id), debugstr_fourcc(chunk.type));
            break;
        }

        if (FAILED(hr)) break;
    }

    if (FAILED(hr)) return hr;

    if (!(entry = calloc(1, sizeof(*entry)))) return E_OUTOFMEMORY;
    memcpy(&entry->instrument, &inst, sizeof(DMUS_IO_INSTRUMENT));
    entry->collection = collection;
    list_add_tail(&This->instruments, &entry->entry);

    return hr;
}

static HRESULT WINAPI band_object_ParseDescriptor(IDirectMusicObject *iface,
        IStream *stream, DMUS_OBJECTDESC *desc)
{
    struct chunk_entry riff = {0};
    STATSTG stat;
    HRESULT hr;

    TRACE("(%p, %p, %p)\n", iface, stream, desc);

    if (!stream || !desc)
        return E_POINTER;

    if ((hr = stream_get_chunk(stream, &riff)) != S_OK)
        return hr;
    if (riff.id != FOURCC_RIFF || riff.type != DMUS_FOURCC_BAND_FORM) {
        TRACE("loading failed: unexpected %s\n", debugstr_chunk(&riff));
        stream_skip_chunk(stream, &riff);
        return DMUS_E_INVALID_BAND;
    }

    hr = dmobj_parsedescriptor(stream, &riff, desc,
            DMUS_OBJ_OBJECT|DMUS_OBJ_NAME|DMUS_OBJ_NAME_INAM|DMUS_OBJ_CATEGORY|DMUS_OBJ_VERSION);
    if (FAILED(hr))
        return hr;

    desc->guidClass = CLSID_DirectMusicBand;
    desc->dwValidData |= DMUS_OBJ_CLASS;

    if (desc->dwValidData & DMUS_OBJ_CATEGORY) {
        IStream_Stat(stream, &stat, STATFLAG_NONAME);
        desc->ftDate = stat.mtime;
        desc->dwValidData |= DMUS_OBJ_DATE;
    }

    TRACE("returning descriptor:\n");
    dump_DMUS_OBJECTDESC(desc);
    return S_OK;
}

static const IDirectMusicObjectVtbl band_object_vtbl =
{
    dmobj_IDirectMusicObject_QueryInterface,
    dmobj_IDirectMusicObject_AddRef,
    dmobj_IDirectMusicObject_Release,
    dmobj_IDirectMusicObject_GetDescriptor,
    dmobj_IDirectMusicObject_SetDescriptor,
    band_object_ParseDescriptor,
};

static HRESULT parse_instruments_list(struct band *This, DMUS_PRIVATE_CHUNK *pChunk,
        IStream *pStm)
{
  HRESULT hr;
  DMUS_PRIVATE_CHUNK Chunk;
  DWORD ListSize[3], ListCount[3];
  LARGE_INTEGER liMove; /* used when skipping chunks */

  if (pChunk->fccID != DMUS_FOURCC_INSTRUMENTS_LIST) {
    ERR_(dmfile)(": %s chunk should be an INSTRUMENTS list\n", debugstr_fourcc (pChunk->fccID));
    return E_FAIL;
  }  

  ListSize[0] = pChunk->dwSize - sizeof(FOURCC);
  ListCount[0] = 0;

  do {
    IStream_Read (pStm, &Chunk, sizeof(FOURCC)+sizeof(DWORD), NULL);
    ListCount[0] += sizeof(FOURCC) + sizeof(DWORD) + Chunk.dwSize;
    TRACE_(dmfile)(": %s chunk (size = %ld)", debugstr_fourcc (Chunk.fccID), Chunk.dwSize);
    switch (Chunk.fccID) {
    case FOURCC_LIST: {
      IStream_Read (pStm, &Chunk.fccID, sizeof(FOURCC), NULL);
      TRACE_(dmfile)(": LIST chunk of type %s", debugstr_fourcc(Chunk.fccID));
      ListSize[1] = Chunk.dwSize - sizeof(FOURCC);
      ListCount[1] = 0;
      switch (Chunk.fccID) { 
      case DMUS_FOURCC_INSTRUMENT_LIST: {
        static const LARGE_INTEGER zero = {0};
        struct chunk_entry chunk = {FOURCC_LIST, .size = Chunk.dwSize, .type = Chunk.fccID};
	TRACE_(dmfile)(": Instrument list\n");
        IStream_Seek(pStm, zero, STREAM_SEEK_CUR, &chunk.offset);
        chunk.offset.QuadPart -= 12;
	if (FAILED(hr = parse_lbin_list(This, pStm, &chunk))) return hr;
	break;
      }
      default: {
	TRACE_(dmfile)(": unknown chunk (irrelevant & skipping)\n");
	liMove.QuadPart = ListSize[1];
	IStream_Seek (pStm, liMove, STREAM_SEEK_CUR, NULL);
	break;						
      }
      }
      break;
    }
    default: {
      TRACE_(dmfile)(": unknown chunk (irrelevant & skipping)\n");
      liMove.QuadPart = Chunk.dwSize;
      IStream_Seek (pStm, liMove, STREAM_SEEK_CUR, NULL);
      break;						
    }
    }
    TRACE_(dmfile)(": ListCount[0] = %ld < ListSize[0] = %ld\n", ListCount[0], ListSize[0]);
  } while (ListCount[0] < ListSize[0]);

  return S_OK;
}

static HRESULT parse_band_form(struct band *This, DMUS_PRIVATE_CHUNK *pChunk,
        IStream *pStm)
{
  HRESULT hr = E_FAIL;
  DMUS_PRIVATE_CHUNK Chunk;
  DWORD StreamSize, StreamCount, ListSize[3], ListCount[3];
  LARGE_INTEGER liMove; /* used when skipping chunks */

  GUID tmp_guid;

  if (pChunk->fccID != DMUS_FOURCC_BAND_FORM) {
    ERR_(dmfile)(": %s chunk should be a BAND form\n", debugstr_fourcc (pChunk->fccID));
    return E_FAIL;
  }  

  StreamSize = pChunk->dwSize - sizeof(FOURCC);
  StreamCount = 0;

  do {
    IStream_Read (pStm, &Chunk, sizeof(FOURCC)+sizeof(DWORD), NULL);
    StreamCount += sizeof(FOURCC) + sizeof(DWORD) + Chunk.dwSize;
    TRACE_(dmfile)(": %s chunk (size = %ld)", debugstr_fourcc (Chunk.fccID), Chunk.dwSize);

    hr = IDirectMusicUtils_IPersistStream_ParseDescGeneric(&Chunk, pStm, &This->dmobj.desc);
    if (FAILED(hr)) return hr;
    
    if (hr == S_FALSE) {
      switch (Chunk.fccID) {
      case DMUS_FOURCC_GUID_CHUNK: {
	TRACE_(dmfile)(": GUID\n");
	IStream_Read (pStm, &tmp_guid, sizeof(GUID), NULL);
	TRACE_(dmfile)(" - guid: %s\n", debugstr_dmguid(&tmp_guid));
	break;
      }
      case FOURCC_LIST: {
	IStream_Read (pStm, &Chunk.fccID, sizeof(FOURCC), NULL);
	TRACE_(dmfile)(": LIST chunk of type %s", debugstr_fourcc(Chunk.fccID));
	ListSize[0] = Chunk.dwSize - sizeof(FOURCC);
	ListCount[0] = 0;
	switch (Chunk.fccID) {
	case DMUS_FOURCC_UNFO_LIST: { 
	  TRACE_(dmfile)(": UNFO list\n");
	  do {
	    IStream_Read (pStm, &Chunk, sizeof(FOURCC)+sizeof(DWORD), NULL);
	    ListCount[0] += sizeof(FOURCC) + sizeof(DWORD) + Chunk.dwSize;
            TRACE_(dmfile)(": %s chunk (size = %ld)", debugstr_fourcc (Chunk.fccID), Chunk.dwSize);

            hr = IDirectMusicUtils_IPersistStream_ParseUNFOGeneric(&Chunk, pStm, &This->dmobj.desc);
	    if (FAILED(hr)) return hr;
	    
	    if (hr == S_FALSE) {
	      switch (Chunk.fccID) {
	      default: {
		TRACE_(dmfile)(": unknown chunk (irrelevant & skipping)\n");
		liMove.QuadPart = Chunk.dwSize;
		IStream_Seek (pStm, liMove, STREAM_SEEK_CUR, NULL);
		break;						
	      }
	      }
	    }
            TRACE_(dmfile)(": ListCount[0] = %ld < ListSize[0] = %ld\n", ListCount[0], ListSize[0]);
	  } while (ListCount[0] < ListSize[0]);
	  break;
	}
	case DMUS_FOURCC_INSTRUMENTS_LIST: {
	  TRACE_(dmfile)(": INSTRUMENTS list\n");
          hr = parse_instruments_list(This, &Chunk, pStm);
	  if (FAILED(hr)) return hr;
	  break;	
	}
	default: {
	  TRACE_(dmfile)(": unknown (skipping)\n");
	  liMove.QuadPart = Chunk.dwSize - sizeof(FOURCC);
	  IStream_Seek (pStm, liMove, STREAM_SEEK_CUR, NULL);
	  break;						
	}
	}
	break;
      }
      default: {
	TRACE_(dmfile)(": unknown chunk (irrelevant & skipping)\n");
	liMove.QuadPart = Chunk.dwSize;
	IStream_Seek (pStm, liMove, STREAM_SEEK_CUR, NULL);
	break;						
      }
      }
    }
    TRACE_(dmfile)(": StreamCount[0] = %ld < StreamSize[0] = %ld\n", StreamCount, StreamSize);
  } while (StreamCount < StreamSize);  

  return S_OK;
}

static inline struct band *impl_from_IPersistStream(IPersistStream *iface)
{
    return CONTAINING_RECORD(iface, struct band, dmobj.IPersistStream_iface);
}

static HRESULT WINAPI band_persist_stream_Load(IPersistStream *iface, IStream *pStm)
{
  struct band *This = impl_from_IPersistStream(iface);
  DMUS_PRIVATE_CHUNK Chunk;
  LARGE_INTEGER liMove;
  HRESULT hr;
  
  TRACE("(%p,%p): loading\n", This, pStm);
  
  IStream_Read (pStm, &Chunk, sizeof(FOURCC)+sizeof(DWORD), NULL);
  TRACE_(dmfile)(": %s chunk (size = %ld)", debugstr_fourcc (Chunk.fccID), Chunk.dwSize);
  switch (Chunk.fccID) {
  case FOURCC_RIFF: {
    IStream_Read (pStm, &Chunk.fccID, sizeof(FOURCC), NULL);
    TRACE_(dmfile)(": %s chunk (size = %ld)", debugstr_fourcc (Chunk.fccID), Chunk.dwSize);
    switch (Chunk.fccID) {
    case DMUS_FOURCC_BAND_FORM: {
      TRACE_(dmfile)(": Band form\n");
      hr = parse_band_form(This, &Chunk, pStm);
      if (FAILED(hr)) return hr;
      break;    
    }
    default: {
      TRACE_(dmfile)(": unexpected chunk; loading failed)\n");
      liMove.QuadPart = Chunk.dwSize;
      IStream_Seek (pStm, liMove, STREAM_SEEK_CUR, NULL);
      return E_FAIL;
    }
    }
    TRACE_(dmfile)(": reading finished\n");
    break;
  }
  default: {
    TRACE_(dmfile)(": unexpected chunk; loading failed)\n");
    liMove.QuadPart = Chunk.dwSize;
    IStream_Seek (pStm, liMove, STREAM_SEEK_CUR, NULL); /* skip the rest of the chunk */
    return E_FAIL;
  }
  }
  
  return S_OK;
}

static const IPersistStreamVtbl band_persist_stream_vtbl =
{
    dmobj_IPersistStream_QueryInterface,
    dmobj_IPersistStream_AddRef,
    dmobj_IPersistStream_Release,
    unimpl_IPersistStream_GetClassID,
    unimpl_IPersistStream_IsDirty,
    band_persist_stream_Load,
    unimpl_IPersistStream_Save,
    unimpl_IPersistStream_GetSizeMax,
};

HRESULT create_dmband(REFIID lpcGUID, void **ppobj)
{
  struct band* obj;
  HRESULT hr;

  *ppobj = NULL;
  if (!(obj = calloc(1, sizeof(*obj)))) return E_OUTOFMEMORY;
  obj->IDirectMusicBand_iface.lpVtbl = &band_vtbl;
  obj->ref = 1;
  dmobject_init(&obj->dmobj, &CLSID_DirectMusicBand, (IUnknown *)&obj->IDirectMusicBand_iface);
  obj->dmobj.IDirectMusicObject_iface.lpVtbl = &band_object_vtbl;
  obj->dmobj.IPersistStream_iface.lpVtbl = &band_persist_stream_vtbl;
  list_init(&obj->instruments);

  hr = IDirectMusicBand_QueryInterface(&obj->IDirectMusicBand_iface, lpcGUID, ppobj);
  IDirectMusicBand_Release(&obj->IDirectMusicBand_iface);

  return hr;
}
