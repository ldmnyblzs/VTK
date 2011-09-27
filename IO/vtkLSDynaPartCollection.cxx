/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkLSDynaReader.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.
=========================================================================*/

#include "vtkLSDynaPartCollection.h"

#include "LSDynaMetaData.h"

#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkDataArray.h"
#include "vtkDoubleArray.h"
#include "vtkIdTypeArray.h"
#include "vtkIntArray.h"
#include "vtkFloatArray.h"
#include "vtkObjectFactory.h"
#include "vtkPoints.h"
#include "vtkPointData.h"
#include "vtkStringArray.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnstructuredGrid.h"

#include <algorithm>
#include <vector>
#include <map>
#include <list>

//-----------------------------------------------------------------------------
namespace
  {
  static const char* TypeNames[] = {
    "PARTICLE",
    "BEAM",
    "SHELL",
    "THICK_SHELL",
    "SOLID",
    "RIGID_BODY",
    "ROAD_SURFACE",
    NULL};

  //stores the mapping from cell to part index and cell index
  struct cellToPartCell
    {
    cellToPartCell(vtkIdType p, vtkIdType c):part(p),cell(c){}
    vtkIdType part;
    vtkIdType cell;
    };

  class cellPropertyInfo
    {
    public:
      cellPropertyInfo(const char* n, const int& sp,
        const vtkIdType &numTuples, const vtkIdType& numComps,
        const int& dataSize):
      StartPos(sp),
      Id(0)
        {
        Data = (dataSize == 4) ? (vtkDataArray*) vtkFloatArray::New() : 
                               (vtkDataArray*) vtkDoubleArray::New();
        Data->SetNumberOfComponents(numComps);
        Data->SetNumberOfTuples(numTuples);
        Data->SetName(n);
        }
      ~cellPropertyInfo()
        {
        Data->Delete();
        }
    int StartPos;
    vtkIdType Id; //Id of the tuple to set next
    vtkDataArray* Data;
    };

  typedef std::map<vtkIdType,vtkIdType> IdTypeMap;

  typedef std::vector<cellToPartCell> CTPCVector;
  typedef std::vector<cellPropertyInfo*> CPIVector;

  typedef std::vector<int> IntVector;
  typedef std::vector<unsigned char> UCharVector;
  typedef std::vector<vtkIdType> IdTypeVector;
  typedef std::vector<vtkLSDynaPartCollection::LSDynaPart*> PartVector;
  typedef std::vector<vtkDataArray*> DataArrayVector;
  }

//-----------------------------------------------------------------------------
class vtkLSDynaPartCollection::LSDynaPart
  {
  public:
  LSDynaPart(LSDynaMetaData::LSDYNA_TYPES t, std::string n):Type(t),Name(n)
    {
    Grid = NULL;
    NextPointId = 0;
    }
  ~LSDynaPart()
    {
    if(Grid)
      {
      Grid->Delete();
      Grid=NULL;
      }
    }

  void InitGrid()
    {
    if(this->Grid != NULL)
      {
      this->Grid->Delete();
      }
    //currently construcutGridwithoutdeadcells calls insertnextcell
    this->Grid = vtkUnstructuredGrid::New();


    //now add in the field data to the grid. Data is the name and type
    vtkFieldData *fd = this->Grid->GetFieldData();

    vtkStringArray *partName = vtkStringArray::New();
    partName->SetName("Name");
    partName->SetNumberOfValues(1);
    partName->SetValue(0,this->Name);
    fd->AddArray(partName);
    partName->FastDelete();

    vtkStringArray *partType = vtkStringArray::New();
    partType->SetName("Type");
    partType->SetNumberOfValues(1);
    partType->SetValue(0,TypeNames[this->Type]);
    fd->AddArray(partType);
    partType->Delete();
    }

  void ResetTimeStepInfo()
    {
    DeadCells.clear();
    CellPropertyInfo.clear();
    }
  
  //Storage of information to build the grid before we call finalize
  //these are constant across all timesteps
  UCharVector CellTypes;
  IdTypeVector CellLocation;
  IdTypeVector CellStructure;
  IdTypeMap PointIds; //maps local point id to global point id
  vtkIdType NextPointId;

  //These need to be cleared every time step
  IntVector DeadCells;
  CPIVector CellPropertyInfo;

  //these are handled by finalize to determine the proper lifespan
  
  //Used to hold the grid representation of this part.
  //Only is valid afer finalize has been called on a timestep
  vtkUnstructuredGrid* Grid;
  

  //Information of the part type
  const LSDynaMetaData::LSDYNA_TYPES Type;
  const std::string Name;
  };

//-----------------------------------------------------------------------------
class vtkLSDynaPartCollection::LSDynaPartStorage
{
public:
  LSDynaPartStorage(const int& size )
    {
    this->CellIndexToPart = new CTPCVector[size];
    }
  ~LSDynaPartStorage()
    {
    delete[] this->CellIndexToPart;
    }

  //Stores the information needed to construct an unstructured grid of the part
  PartVector Parts;

  //maps cell indexes which are tracked by output type to the part
  //Since cells are ordered the same between the cell connectivity data block
  //and the state block in the d3plot format we only need to know which part
  //the cell is part of.
  //This info is constant for each time step so it can't be cleared.
  CTPCVector *CellIndexToPart; 

  //Stores all the point properties for all the parts.
  //When we finalize each part we will split these property arrays up
  DataArrayVector PointProperties;
};

vtkStandardNewMacro(vtkLSDynaPartCollection);
//-----------------------------------------------------------------------------
vtkLSDynaPartCollection::vtkLSDynaPartCollection():
  Finalized(false)
{
  this->MetaData = NULL;
  this->Storage = NULL;
  this->MinIds = NULL;
  this->MaxIds = NULL;
}

//-----------------------------------------------------------------------------
vtkLSDynaPartCollection::~vtkLSDynaPartCollection()
{
  PartVector::iterator it;
  for(it=this->Storage->Parts.begin();
      it!=this->Storage->Parts.end();
      ++it)
    {
    if(*it)
      {
      (*it)->ResetTimeStepInfo();
      delete (*it);
      (*it)=NULL;
      }
    }
  if(this->Storage)
    {
    this->Storage->Parts.clear();
    delete this->Storage;
    }

  if(this->MinIds)
    {
    delete[] this->MinIds;
    }
  if(this->MaxIds)
    {
    delete[] this->MaxIds;
    }
  this->MetaData = NULL;
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::PrintSelf(ostream &os, vtkIndent indent)
{

}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::InitCollection(LSDynaMetaData *metaData,
  vtkIdType* mins, vtkIdType* maxs)
{
  if(this->Storage)
    {
    this->Storage->Parts.clear();
    delete this->Storage;
    }

  if(this->MinIds)
    {
    delete[] this->MinIds;
    }
  if(this->MaxIds)
    {
    delete[] this->MaxIds;
    }

  this->Storage = new LSDynaPartStorage(LSDynaMetaData::NUM_CELL_TYPES);
  this->MinIds = new vtkIdType[LSDynaMetaData::NUM_CELL_TYPES];
  this->MaxIds = new vtkIdType[LSDynaMetaData::NUM_CELL_TYPES];

  //reserve enough space for cell index to part.  We only
  //have to map the cell ids between min and max, so we
  //skip into the proper place
  cellToPartCell t(-1,-1);
  for(int i=0; i < LSDynaMetaData::NUM_CELL_TYPES;++i)
    {
    this->MinIds[i]= (mins!=NULL) ? mins[i] : 0;
    this->MaxIds[i]= (maxs!=NULL) ? maxs[i] : metaData->NumberOfCells[i];
    const vtkIdType reservedSpaceSize(this->MaxIds[i]-this->MinIds[i]);
    this->Storage->CellIndexToPart[i].resize(reservedSpaceSize,t);
    }

  if(metaData && !this->Finalized)
    {
    this->MetaData = metaData;
    this->BuildPartInfo(mins,maxs);
    }
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::BuildPartInfo(vtkIdType* mins, vtkIdType* maxs)
{
  //reserve enough space for the grids. Each node
  //will have a part allocated, since we don't know yet
  //how the cells map to parts.
  size_t size = this->MetaData->PartIds.size();
  this->Storage->Parts.resize(size,NULL);

  //we iterate on part materials as those are those are from 1 to num Parts.
  //the part ids are the user part numbers
  std::vector<int>::const_iterator partMIt;
  std::vector<int>::const_iterator statusIt = this->MetaData->PartStatus.begin();
  std::vector<LSDynaMetaData::LSDYNA_TYPES>::const_iterator typeIt = this->MetaData->PartTypes.begin();
  std::vector<std::string>::const_iterator nameIt = this->MetaData->PartNames.begin();

  for (partMIt = this->MetaData->PartMaterials.begin();
       partMIt != this->MetaData->PartMaterials.end();
       ++partMIt,++statusIt,++typeIt,++nameIt)
    {
    if (*statusIt)
      {
      //make the index contain a part
      this->Storage->Parts[*partMIt-1] =
      new vtkLSDynaPartCollection::LSDynaPart(*typeIt,*nameIt);
      }
    }  
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::InsertCell(const int& partType,
                                         const vtkIdType& cellIndex,
                                         const vtkIdType& matId,
                                         const int& cellType,
                                         const vtkIdType& npts,
                                         vtkIdType conn[8])
{
  if (this->Finalized)
    {
    //you cant add cells after calling finalize
    return;
    }

  vtkLSDynaPartCollection::LSDynaPart *part = this->Storage->Parts[matId-1];
  if (!part)
    {
    return;
    }

  //push back the cell into the proper part grid for storage
  part->CellTypes.push_back(static_cast<unsigned char>(cellType));  
  
  part->CellStructure.push_back(npts);
  //compute the direct postion this is needed when we finalize the data into
  //a unstructured grid. We need to find the size after we push back the npts
  part->CellLocation.push_back(
    static_cast<vtkIdType>(part->CellStructure.size()-1));
  
  //now push back the rest of the cell structure
  for(int i=0; i<npts; ++i)
    {
    //LSDyna usin Fortran indexes (starts at 1)
    part->CellStructure.push_back(conn[i]-1);
    }


  this->Storage->CellIndexToPart[partType][cellIndex] =
    cellToPartCell(matId-1,part->CellTypes.size()-1); 
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::SetCellDeadFlags(
                                      const int& partType, vtkIntArray *death)
{
  //go through and flag each part cell as deleted or not.
  //this means breaking up this array into an array for each part
  if (!death)
    {
    return;
    }
  if(this->Storage->CellIndexToPart[partType].size() == 0)
    {
    //we aren't storing any cells for this part so ignore it
    return;
    }

  //The array that passed in from the reader only contains the subset
  //of the full data that we are interested in so we don't have to adjust
  //any indices
  vtkIdType size = death->GetNumberOfTuples();
  int deleted = 0;
  for(vtkIdType i=0;i<size;++i)
    {
    cellToPartCell pc =  this->Storage->CellIndexToPart[partType][i];
    deleted = death->GetValue(i);
    if(deleted && pc.part > -1)
      {
      //only store the deleted cells.
      this->Storage->Parts[pc.part]->DeadCells.push_back(pc.cell);
      }
    }
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::AddPointArray(vtkDataArray* data)
{
  this->Storage->PointProperties.push_back(data);
}

//-----------------------------------------------------------------------------
int vtkLSDynaPartCollection::GetNumberOfPointArrays() const
{
  return static_cast<int>(this->Storage->PointProperties.size());
}

//-----------------------------------------------------------------------------
vtkDataArray* vtkLSDynaPartCollection::GetPointArray(const int& index) const
{
  if ( index < 0 || index >= this->GetNumberOfPointArrays())
    {
    return NULL;
    }
  return this->Storage->PointProperties[index];
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::AddProperty(
                    const LSDynaMetaData::LSDYNA_TYPES& type, const char* name,
                    const int& offset, const int& numComps)
{
  vtkIdType numTuples=0;
  PartVector::iterator partIt;
  for (partIt = this->Storage->Parts.begin();
       partIt != this->Storage->Parts.end();
       ++partIt)
    {
    vtkLSDynaPartCollection::LSDynaPart* part = *partIt;    
    if (part && part->Type == type)
      {
      numTuples = part->CellTypes.size();
      cellPropertyInfo* t = new cellPropertyInfo(name,offset,numTuples,numComps,
        this->MetaData->Fam.GetWordSize());
      part->CellPropertyInfo.push_back(t);
      }
    }
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::FillCellProperties(float *buffer,
  const LSDynaMetaData::LSDYNA_TYPES& type, const vtkIdType& numCells,
  const int& numTuples)
{
  this->FillCellArray(buffer,type,numCells,numTuples);
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::FillCellProperties(double *buffer,
  const LSDynaMetaData::LSDYNA_TYPES& type, const vtkIdType& numCells,
  const int& numTuples)
{
  this->FillCellArray(buffer,type,numCells,numTuples);
}

//-----------------------------------------------------------------------------
template<typename T>
void vtkLSDynaPartCollection::FillCellArray(T *buffer,
  const LSDynaMetaData::LSDYNA_TYPES& type, const vtkIdType& numCells,
  const int& numTuples)
{
  //we only need to iterate the array for the subsection we need
  if(this->Storage->CellIndexToPart[type].size() == 0)
    {
    return;
    }

  vtkIdType size = this->MaxIds[type] - this->MinIds[type];
  for(vtkIdType i=0;i<size;++i)
    {
    cellToPartCell pc = this->Storage->CellIndexToPart[type][i];
    if(pc.part > -1)
      {
      //read the next chunk from the buffer
      T* tuple = &buffer[i*numTuples];      

      //take that chunk and move it to the properties that are active
      vtkLSDynaPartCollection::LSDynaPart* part = this->Storage->Parts[pc.part];
      if(!part)
        {
        continue;
        }
      CPIVector::iterator it;
      for(it=part->CellPropertyInfo.begin();
        it!=part->CellPropertyInfo.end();
        ++it)
        {
        //set the next tuple in this property.
        //start pos is the offset in the data
        (*it)->Data->SetTuple((*it)->Id++,tuple + (*it)->StartPos);
        }

      }
    }
}

//-----------------------------------------------------------------------------
bool vtkLSDynaPartCollection::IsActivePart(const int& id) const
{
  if (id < 0 || id >= this->Storage->Parts.size())
    {
    return false;
    }

  return this->Storage->Parts[id] != NULL;
}

//-----------------------------------------------------------------------------
vtkUnstructuredGrid* vtkLSDynaPartCollection::GetGridForPart(
  const int& index) const
{

  if (!this->Finalized)
    {
    //you have to call finalize first
    return NULL;
    }

  return this->Storage->Parts[index]->Grid;
}

//-----------------------------------------------------------------------------
int vtkLSDynaPartCollection::GetNumberOfParts() const
{
  return static_cast<int>(this->Storage->Parts.size());
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::GetPartReadInfo(const int& partType,
  vtkIdType& numberOfCells, vtkIdType& numCellsToSkipStart,
  vtkIdType& numCellsToSkipEnd) const
{
  if(this->Storage->CellIndexToPart[partType].size() == 0)
    {
    numberOfCells = 0;
    //skip everything
    numCellsToSkipStart = this->MetaData->NumberOfCells[partType];
    numCellsToSkipEnd = 0; //no reason to skip anything else
    }
  else
    {
    numberOfCells = this->Storage->CellIndexToPart[partType].size();
    numCellsToSkipStart = this->MinIds[partType];
    numCellsToSkipEnd = this->MetaData->NumberOfCells[partType] -
                                        (numberOfCells+numCellsToSkipStart);
    }
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::FinalizeTopology()
{
  //we are going to take all the old point ids and convert them to the new
  //ids based on the point subset required for this topology.

  //If you use a map while inserting cells you get really really bad performance
  //instead we will create a lookup table of old ids to new ids. From that
  //we will create a reduced set of pairs in sorted order. those sorted pairs
  //will be used to create the map which means it the map will be constructed
  //in linear time.

  //Note the trade off here is that removing dead points will be really hard,
  //so we wont!

  //we are making the PointId map be new maps to old.

  std::list< std::pair<vtkIdType,vtkIdType> > inputToMap;
  IdTypeVector lookup;
  lookup.resize(this->MetaData->NumberOfNodes,-1);

  PartVector::iterator partIt;

  //make sure to only build topology info the parts we are loading
  vtkIdType index = 0;
  for (partIt = this->Storage->Parts.begin();
       partIt != this->Storage->Parts.end();
       ++partIt,++index)
    {
    if((*partIt) && (*partIt)->CellStructure.size() == 0)
      {
      //this part wasn't valid given the cells we read in so we have to remove it
      //this is really only happens when running in parallel and if a node
      //is reading all the cells for a part, all other nodes will than delete that part
      delete (*partIt);
      (*partIt)=NULL;
      }
    else if (*partIt)
      {
      inputToMap.clear();

      //take the cell array and find all the unique points
      //once that is done convert them into a map
      
      vtkIdType nextPointId=0, npts=0;
      IdTypeVector::iterator csIt;
      
      for(csIt=(*partIt)->CellStructure.begin();
          csIt!=(*partIt)->CellStructure.end();)
        {
        npts = (*csIt);
        ++csIt; //move to the first point for this cell
        for(vtkIdType i=0;i<npts;++i,++csIt)
          {
          if(lookup[*csIt] == -1)
            {
            //update the lookup table
            std::pair<vtkIdType,vtkIdType> pair(nextPointId,*csIt);
            lookup[*csIt] = nextPointId;            
            ++nextPointId;
            inputToMap.push_back(pair);
            }
          *csIt = lookup[*csIt];
          }
        }

      //create the mapping from new ids to old ids for the points
      //this constructor will be linear time since the list is already sorted
      (*partIt)->PointIds = IdTypeMap(inputToMap.begin(),inputToMap.end());

      //reset the lookup table
      std::fill(lookup.begin(),lookup.end(),-1);
      }
    }
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::Finalize(vtkPoints *commonPoints,
  const int& removeDeletedCells)
{
  PartVector::iterator partIt;
  for (partIt = this->Storage->Parts.begin();
       partIt != this->Storage->Parts.end();
       ++partIt)
    {
    if ( (*partIt))
      {
      (*partIt)->InitGrid();
      if(removeDeletedCells && (*partIt)->DeadCells.size() > 0)
        {
        this->ConstructGridCellsWithoutDeadCells(*partIt);
        }
      else
        {
        this->ConstructGridCells(*partIt);
        }
      //now construct the points for the grid
      this->ConstructGridPoints(*partIt,commonPoints);
      }
    }

  this->ResetTimeStepInfo();
  this->Finalized = true;
}


//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::ConstructGridCells(LSDynaPart *part)
{  
  if(part->CellTypes.size() == 0 )
    {
    //the part is empty
    return;
    }  
  
  //needed info
  vtkIdType numCells = static_cast<vtkIdType>(part->CellTypes.size());
  vtkIdType sizeOfCellStruct = static_cast<vtkIdType>(part->CellStructure.size());

  //copy the contents from the part into a cell array.  
  vtkIdTypeArray *cellArray = vtkIdTypeArray::New();
  cellArray->SetNumberOfValues(sizeOfCellStruct);
  std::copy(part->CellStructure.begin(),part->CellStructure.end(),
    reinterpret_cast<vtkIdType*>(cellArray->GetVoidPointer(0)));

  //set the idtype aray as the cellarray
  vtkCellArray *cells = vtkCellArray::New();
  cells->SetCells(numCells,cellArray);
  cellArray->FastDelete();

  //now copy the cell types from the vector to 
  vtkUnsignedCharArray* cellTypes = vtkUnsignedCharArray::New();
  cellTypes->SetNumberOfValues(numCells);
  std::copy(part->CellTypes.begin(),part->CellTypes.end(),
     reinterpret_cast<unsigned char*>(cellTypes->GetVoidPointer(0)));

  //last is the cell locations
  vtkIdTypeArray *cellLocation = vtkIdTypeArray::New();
  cellLocation->SetNumberOfValues(numCells);
  std::copy(part->CellLocation.begin(),part->CellLocation.end(),
     reinterpret_cast<vtkIdType*>(cellLocation->GetVoidPointer(0)));

  //actually set up the grid
  part->Grid->SetCells(cellTypes,cellLocation,cells,NULL,NULL);

  //remove references
  cellTypes->FastDelete();
  cellLocation->FastDelete();
  cells->FastDelete();

  //now copy the cell data into the part
  vtkCellData *gridData = part->Grid->GetCellData();
  CPIVector::iterator it;
  for(it=part->CellPropertyInfo.begin();
      it!=part->CellPropertyInfo.end();
      ++it)
      {
      gridData->AddArray((*it)->Data);
      (*it)->Data->FastDelete();
      }
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::ConstructGridCellsWithoutDeadCells(LSDynaPart *part)
{
  if(part->CellTypes.size() == 0 )
    {
    //the part is empty
    return;
    }
  vtkUnstructuredGrid *grid = part->Grid;
  vtkIdType numCells = static_cast<vtkIdType>(part->CellTypes.size());
  vtkIdType numDeadCells = part->DeadCells.size();

  //setup the cell properties
  CPIVector::iterator oldArrayIt;
  DataArrayVector::iterator newArrayIt;
  
  DataArrayVector newArrays;
  newArrays.resize(part->CellPropertyInfo.size());

  oldArrayIt = part->CellPropertyInfo.begin();
  vtkCellData *cd = grid->GetCellData();
  for(newArrayIt=newArrays.begin();newArrayIt!=newArrays.end();
    ++newArrayIt,++oldArrayIt)
    {
    vtkDataArray *d = (*oldArrayIt)->Data;
    (*newArrayIt)=d->NewInstance();
    (*newArrayIt)->SetName(d->GetName());
    (*newArrayIt)->SetNumberOfComponents(d->GetNumberOfComponents());
    (*newArrayIt)->SetNumberOfTuples(numCells-numDeadCells);

    cd->AddArray(*newArrayIt);
    (*newArrayIt)->FastDelete();
    }
 
  //this has a totally different method since we can't use the clean implementation
  //of copying the memory right from the vectors. Instead we have to skip
  //the chunks that have been deleted. For te cell location and cell types this
  //is fairly easy for the cell structure it is a bit more complicated

  //needed infos  
  vtkIdType currentDeadCellPos=0;
  UCharVector::iterator typeIt = part->CellTypes.begin();
  IdTypeVector::iterator locIt = part->CellLocation.begin();
  vtkIdType i=0,idx=0;
  for(; i<numCells && currentDeadCellPos<numDeadCells ;++i,++typeIt,++locIt)
    {
    if(part->DeadCells[currentDeadCellPos] != i)
      {
      grid->InsertNextCell(*typeIt,part->CellStructure[*locIt],&part->CellStructure[*locIt+1]);

      oldArrayIt = part->CellPropertyInfo.begin();
      for(newArrayIt=newArrays.begin();
      newArrayIt!=newArrays.end();
      ++newArrayIt,++oldArrayIt)
        {
        (*newArrayIt)->SetTuple(idx, (*oldArrayIt)->Data->GetTuple(i));
        }
      ++idx;
      }
    else
      {
      ++currentDeadCellPos;
      }
    }

  //we have all the dead cells tight loop the rest
  for(; i<numCells;++i,++typeIt,++locIt)
    {
    grid->InsertNextCell(*typeIt,part->CellStructure[*locIt],&part->CellStructure[*locIt+1]);
    oldArrayIt = part->CellPropertyInfo.begin();
    for(newArrayIt=newArrays.begin();
      newArrayIt!=newArrays.end();
      ++newArrayIt,++oldArrayIt)
      {
      (*newArrayIt)->SetTuple(idx, (*oldArrayIt)->Data->GetTuple(i));
      }
    ++idx;        
    }

}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::ConstructGridPoints(LSDynaPart *part, vtkPoints *commonPoints)
{
  vtkIdType size = part->PointIds.size();

  //now compute the points for the grid
  vtkPoints *points = vtkPoints::New();
  points->SetNumberOfPoints(size);

  //create new property arrays
  DataArrayVector::iterator newArrayIt, ppArrayIt;
  DataArrayVector newArrays;
  newArrays.resize(this->Storage->PointProperties.size(),NULL);
  ppArrayIt = this->Storage->PointProperties.begin();
  for(newArrayIt=newArrays.begin();newArrayIt!=newArrays.end();
    ++newArrayIt,++ppArrayIt)
    {
    (*newArrayIt)=(*ppArrayIt)->NewInstance();
    (*newArrayIt)->SetName((*ppArrayIt)->GetName());
    (*newArrayIt)->SetNumberOfComponents((*ppArrayIt)->GetNumberOfComponents());
    (*newArrayIt)->SetNumberOfTuples(size);

    part->Grid->GetPointData()->AddArray((*newArrayIt));
    (*newArrayIt)->FastDelete();
    }

  //fill the points and point property classes
  IdTypeMap::const_iterator pIt;
  for(pIt=part->PointIds.begin();
      pIt!=part->PointIds.end();
      ++pIt)
    {
    //set the point
    points->SetPoint(pIt->first,commonPoints->GetPoint(pIt->second));

    //set the properties for the point
    for(newArrayIt=newArrays.begin(),ppArrayIt=this->Storage->PointProperties.begin();
            newArrayIt!=newArrays.end();
            ++newArrayIt,++ppArrayIt)
        {
        (*newArrayIt)->SetTuple(pIt->first, (*ppArrayIt)->GetTuple(pIt->second));
        }
    }

  part->Grid->SetPoints(points);
  points->FastDelete();
}

//-----------------------------------------------------------------------------
void vtkLSDynaPartCollection::ResetTimeStepInfo()
{
  PartVector::iterator it;
  for(it=this->Storage->Parts.begin();
      it!=this->Storage->Parts.end();
      ++it)
    {
    if(*it)
      {
      (*it)->ResetTimeStepInfo();
      }
    }

  //delete all the point properties in the global form
  DataArrayVector::iterator doIt;
  for(doIt=this->Storage->PointProperties.begin();
    doIt!=this->Storage->PointProperties.end();
    ++doIt)
    {
    vtkDataArray* da = vtkDataArray::SafeDownCast(*doIt);
    da->Delete();
    }
  this->Storage->PointProperties.clear();

  this->Finalized = false;
}
