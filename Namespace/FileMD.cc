//------------------------------------------------------------------------------
// author: Lukasz Janyst <ljanyst@cern.ch>
// desc:   Class representing the file metadata
//------------------------------------------------------------------------------

#include "Namespace/FileMD.hh"
#include "Namespace/IFileMDSvc.hh"

namespace eos
{
  //----------------------------------------------------------------------------
  // Constructor
  //----------------------------------------------------------------------------
  FileMD::FileMD( id_t id, IFileMDSvc *fileMDSvc ):
    pId( id ),
    pSize( 0 ),
    pContainerId( 0 ),
    pCUid( 0 ),
    pCGid( 0 ),
    pLayoutId( 0 ),
    pFileMDSvc( fileMDSvc )
  {
    pCTime.tv_sec = pCTime.tv_nsec = 0;
    pMTime.tv_sec = pMTime.tv_nsec = 0;
  }

  //----------------------------------------------------------------------------
  // Add location
  //----------------------------------------------------------------------------
  void FileMD::addLocation( location_t location )
  {
    if (hasLocation(location))
      return;
    pLocation.push_back( location );
    IFileMDChangeListener::Event e( this,
                                    IFileMDChangeListener::LocationAdded,
                                    location );
    pFileMDSvc->notifyListeners( &e );
  }

  //----------------------------------------------------------------------------
  // replace location by index
  //----------------------------------------------------------------------------
  void FileMD::replaceLocation( unsigned int index, location_t newlocation )
  {
    location_t oldLocation = pLocation[index];
    pLocation[index] = newlocation;
    IFileMDChangeListener::Event e( this,
                                    IFileMDChangeListener::LocationReplaced,
                                    newlocation, oldLocation );
    pFileMDSvc->notifyListeners( &e );
  }

  //----------------------------------------------------------------------------
  // Remove location
  //----------------------------------------------------------------------------
  void FileMD::removeLocation( location_t location )
  {
    std::vector<location_t>::iterator it;
    for( it = pUnlinkedLocation.begin(); it < pUnlinkedLocation.end(); ++it)
    {
      if (*it == location)
      {
        pUnlinkedLocation.erase(it);
        IFileMDChangeListener::Event e( this,
                                        IFileMDChangeListener::LocationRemoved,
                                        location );
        pFileMDSvc->notifyListeners( &e );
        return;
      }
    }
  }

  //----------------------------------------------------------------------------
  // Unlink location
  //----------------------------------------------------------------------------
  void FileMD::unlinkLocation( location_t location )
  {
    std::vector<location_t>::iterator it;
    for ( it=pLocation.begin() ; it < pLocation.end(); it++ ) {
      if (*it == location)
      {
        pUnlinkedLocation.push_back( *it );
        pLocation.erase( it );
        IFileMDChangeListener::Event e( this,
                                        IFileMDChangeListener::LocationUnlinked,
                                        location );
        pFileMDSvc->notifyListeners( &e );
        return;
      }
    }
  }

  //----------------------------------------------------------------------------
  // Unlink all locations
  //----------------------------------------------------------------------------
  void FileMD::unlinkAllLocations()
  {
    std::vector<location_t>::iterator it;
    for( it = pLocation.begin(); it < pLocation.end(); ++it )
    {
      pUnlinkedLocation.push_back( *it );
      IFileMDChangeListener::Event e( this,
                                      IFileMDChangeListener::LocationUnlinked,
                                      *it );
      pFileMDSvc->notifyListeners( &e );
    }
    pLocation.clear();
  }

  //------------------------------------------------------------------------
  //  Env Representation
  //------------------------------------------------------------------------
  void FileMD::getEnv( std::string &env ) 
  {
    env="";
    char senv[16384];
    sprintf(senv,"name=%s&id=%llu&ctime=%llu&ctime_ns=%llu&mtime=%llu&mtime_ns=%llu&size=%llu&cid=%llu&uid=%lu&gid=%lu&lid=%lu"
	    ,pName.c_str()
	    ,(unsigned long long) pId
	    ,(unsigned long long) pCTime.tv_sec
	    ,(unsigned long long) pCTime.tv_nsec
	    ,(unsigned long long) pMTime.tv_sec
	    ,(unsigned long long) pMTime.tv_nsec
	    ,(unsigned long long) pSize
	    ,(unsigned long long) pContainerId
	    ,(unsigned long) pCUid
	    ,(unsigned long) pCGid
	    ,(unsigned long) pLayoutId);

    env += senv;
    env += "&location=";
    LocationVector::iterator it;
    for( it = pLocation.begin(); it != pLocation.end(); ++it )
      {
	env += (int) *it;
	env += ",";
      }
    
    env += "&checksum=";
    uint8_t size = pChecksum.getSize();
    for ( uint8_t i=0; i < size; i++ ) {
      char hx[3];
      sprintf(hx,"%02x",*(pChecksum.getDataPtr()+i));
      env += hx;
    }
  }


  //----------------------------------------------------------------------------
  // Serialize the object to a buffer
  //----------------------------------------------------------------------------
  void FileMD::serialize( Buffer &buffer ) throw( MDException )
  {
    buffer.putData( &pId,          sizeof( pId ) );
    buffer.putData( &pCTime,       sizeof( pCTime ) );
    buffer.putData( &pMTime,       sizeof( pMTime ) );
    buffer.putData( &pSize,        sizeof( pSize ) );
    buffer.putData( &pContainerId, sizeof( pContainerId ) );

    uint16_t len = pName.length()+1;
    buffer.putData( &len,          sizeof( len ) );
    buffer.putData( pName.c_str(), len );

    len = pLocation.size();
    buffer.putData( &len, sizeof( len ) );

    LocationVector::iterator it;
    for( it = pLocation.begin(); it != pLocation.end(); ++it )
    {
      location_t location = *it;
      buffer.putData( &location, sizeof( location_t ) );
    }

    len = pUnlinkedLocation.size();
    buffer.putData( &len, sizeof( len ) );

    for( it = pUnlinkedLocation.begin(); it != pUnlinkedLocation.end(); ++it )
    {
      location_t location = *it;
      buffer.putData( &location, sizeof( location_t ) );
    }

    buffer.putData( &pCUid,      sizeof( pCUid ) );
    buffer.putData( &pCGid,      sizeof( pCGid ) );
    buffer.putData( &pLayoutId, sizeof( pLayoutId ) );

    uint8_t size = pChecksum.getSize();
    buffer.putData( &size, sizeof( size ) );
    buffer.putData( pChecksum.getDataPtr(), size );
  }

  //----------------------------------------------------------------------------
  // Deserialize the class to a buffer
  //----------------------------------------------------------------------------
  void FileMD::deserialize( const Buffer &buffer ) throw( MDException )
  {
    uint16_t offset = 0;
    offset = buffer.grabData( offset, &pId,          sizeof( pId ) );
    offset = buffer.grabData( offset, &pCTime,       sizeof( pCTime ) );
    offset = buffer.grabData( offset, &pMTime,       sizeof( pMTime ) );
    offset = buffer.grabData( offset, &pSize,        sizeof( pSize ) );
    offset = buffer.grabData( offset, &pContainerId, sizeof( pContainerId ) );

    uint16_t len = 0;
    offset = buffer.grabData( offset, &len, 2 );
    char strBuffer[len];
    offset = buffer.grabData( offset, strBuffer, len );
    pName = strBuffer;

    offset = buffer.grabData( offset, &len, 2 );
    for( uint16_t i = 0; i < len; ++i )
    {
      location_t location;
      offset = buffer.grabData( offset, &location, sizeof( location_t ) );
      pLocation.push_back( location );
    }

    offset = buffer.grabData( offset, &len, 2 );
    for( uint16_t i = 0; i < len; ++i )
    {
      location_t location;
      offset = buffer.grabData( offset, &location, sizeof( location_t ) );
      pUnlinkedLocation.push_back( location );
    }

    offset = buffer.grabData( offset, &pCUid,      sizeof( pCUid ) );
    offset = buffer.grabData( offset, &pCGid,      sizeof( pCGid ) );
    offset = buffer.grabData( offset, &pLayoutId, sizeof( pLayoutId ) );

    uint8_t size = 0;
    offset = buffer.grabData( offset, &size, sizeof( size ) );
    pChecksum.resize( size );
    offset = buffer.grabData( offset, pChecksum.getDataPtr(), size );
  };
}
