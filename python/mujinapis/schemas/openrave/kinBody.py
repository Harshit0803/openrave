from mujincommon.schema.basicSchemas import MakePoseSchema

from .grabbedInfo import grabbedInfoSchema
from .jointInfo import jointInfoSchema
from .linkInfo import linkInfoSchema
from .readableInterface import readableInterfacesSchema

from . import _

bodySchema = {  # TODO(felixvd): Link from openrave kinbody
    "type": "object",
    "typeName": "KinBody",
    "properties":{
        "id": {"type": "string"},
        "name": {"type": "string"},
        "referenceUri": {"type": "string"},
        "referenceUriHint": {"type": "string"},
        "interfaceType": {"type": "string"},
        "transform": MakePoseSchema(title=_("transform"), description=_("transform")),
        "isRobot": {"type": "boolean"},
        "dofValues": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "jointName": {"type": "string"},
                    "jointAxis": {"type": "number"},
                    "value": {"type": "number"},
                },
            },
        },
        "grabbed": {
            "type": "array",
            "items": grabbedInfoSchema,
        },
        "links": {
            "type": "array",
            "items": linkInfoSchema,
        },
        "joints": {
            "type": "array",
            "items": jointInfoSchema,
        },
        "readableInterfaces": readableInterfacesSchema,
        "__isPartial__": {"type": "boolean"},
        "files": {"type": "object"},
    }
}